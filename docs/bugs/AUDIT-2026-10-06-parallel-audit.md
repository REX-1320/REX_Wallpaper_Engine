# AUDIT-2026-10-06 — parallel subsystem audit (4 agents, findings verified)

> Follow-up to [`BUG-001-workerw-zorder-input-hang.md`](BUG-001-workerw-zorder-input-hang.md).
> Method: 4 read-only `explore` agents, one per independent domain (engine lifecycle / D3D12 /
> Electron main / module platform), then **manual re-verification of every high-severity claim**
> against the cited lines. Verified items were re-read by hand; unverified ones are marked
> ⚠️ *agent-reported, not re-read*.
> Scope: bug hunting only — nothing was fixed here.

Legend: **V** = verified by hand · ⚠️ = agent report only

---

## Shipped-path bugs (affect `rex-wallpaper-runtime.exe` / the Electron app)

### A1. CRITICAL · V — Use-after-free: telemetry thread vs. render-thread recovery
`Engine::GetTelemetry` walks `m_videoManager` / `m_renderer` **without any lock**
(`Engine.cpp:867-891`), while `ReinitializeRenderer` `reset()`s both
(`Engine.cpp:234-241`, `:258`) on the render thread. The runtime's 5-second telemetry
thread (`wallpaper_cli_main.cpp:645`, guarded by `engineGuard` — a *different* mutex)
therefore dereferences freed objects during every Explorer restart / TDR recovery.
Also `m_videoManager->GetSlotHandle` (`:864`) is unlocked.
**Fix direction:** recovery must not destroy objects other threads can reach — either
share_ptr + copy-out, or take `m_mutex` around the teardown (and keep `GetTelemetry`
lock-free by snapshotting atomics).

### A2. CRITICAL · V — Device-loss path hot-spins at 100% CPU and can brick the engine
`m_deviceRemoved` is set in 13 places and **never cleared** (grep: no `= false` anywhere).
`ReinitializeRenderer` calls `m_renderer->Shutdown()` *before* `Acquire()`, and on
`Acquire()` failure returns early (`Engine.cpp:239-252`) leaving the old renderer in
place. `RenderLoop` then re-enters `HandleDeviceLoss()` every iteration and `continue`s
**without pacing** (`Engine.cpp:1249-1259`): `m_frameCount` never increments, so
`CheckFullScreenPause()` and `CheckDisplayChanges()` (`EnumDisplayMonitors`) also run every
iteration. If recovery instead fails at renderer re-init, `m_renderer` is a fresh
uninitialized object → the loop sleeps 16 ms forever, `m_recoveryNeeded` was already
consumed, state stays `Error` → **no command can recover the wallpaper; process restart only**.
**Fix direction:** clear the loss flag in `Shutdown()`/on successful `Initialize`, pace the
retry loop, and re-arm `m_recoveryNeeded` on failure.

### A3. MAJOR · V — Uncommitted WIP regresses SDR-on-HDR brightness ~500×
The uncommitted diff replaced the paper-white divisor with the mastering-max divisor in the
**non-PQ** HDR-output branch: `lin = rgb * norm` (`D3D12Renderer.cpp:245`) where
`norm = masterMax = disp.y` (`:142,:150`). `cb[9]` is `masteringDisplayMaxLuminance`
(`:1628`), which the decoder leaves at its **1000.0 default for SDR content**
(`FfmpegVideoDecoder.hpp:45`, only overwritten for non-SDR at `FfmpegVideoDecoder.cpp:556-557`).
→ SDR video on an HDR output is scaled by 1000 instead of paper white (203) → blown-out
white. `cb[7] = m_paperWhiteNits` (`:1624`) is still written but no longer read, i.e.
`SetPaperWhiteNits` became a no-op. **Probable relation to the existing "HDR present bug"
backlog item** — worth testing together.

### A4. MAJOR · V — `CopyTextureRegion` into a texture left in `PIXEL_SHADER_RESOURCE`
`m_texture` is created in `COPY_DEST` (`:883`), each upload copies into it and transitions
`COPY_DEST → PIXEL_SHADER_RESOURCE` *after* the copy (`:989-997`), and **nothing transitions
it back** before the next upload. From the second upload on, the copy destination is in the
wrong state → D3D12 debug-layer error and undefined copy result. Also `WaitForGPU()` per
upload (`:1001`) stalls the whole queue. (Video path does this correctly — `:952-958`.)

### A5. MAJOR · V — `PAUSE` / `STOP` / `UNLOAD` first *play*
`wallpaper_cli_main.cpp:917-935`: `engine->Play()` is called unconditionally, then
overwritten by the real call. `Play()` restarts the stopwatch and resumes decode/audio for a
moment, and resets playback position to ~0. So a PAUSE reports position 0.0 afterwards.

### A6. MAJOR · V — Engine scene/hot-reload state read racily by the render thread
`HandleDeviceLoss` copies `m_currentScene` (3 `std::string`s) with no lock (`Engine.cpp:300`),
and `RenderLoop` reads `m_currentScene.type`, `m_hotReloadPath` (`std::string`) and
`m_audioCapture` (`unique_ptr`) unlocked, while the command thread assigns them under
`m_mutex`. `m_mutex` gives no protection — the render thread never takes it.
⚠️ Render-loop read sites cited by the agent: `Engine.cpp:1272,1312,1315-1318,1326`.

### A7. MAJOR · V — Packaged build ships **no tray icon** → app unreachable
`app/package.json` `files: ["dist/**","electron/**"]` omits `assets/`, but the only tray icon
is `path.join(__dirname,'..','assets','rex-tray.ico')` (`tray-manager.cjs:12`). The window's
close handler hides it and `window-all-closed` is a no-op (`main.cjs:179-182,428`), so Exit
lives **only** in the tray menu → after `pnpm dist:win` the user cannot restore or quit REX
and must End-Task it, orphaning the wallpaper runtime.

### A8. MAJOR · ⚠️ — Crash-recovery respawn storm (Electron)
`recoverRuntime()` restarts its 5-attempt budget per `exit` event with no re-entrancy guard
(`main.cjs:104-124,127-131`); the only circuit breaker (`supervisor/supervisor.cjs`, max 3
attempts) is never required. A wallpaper that always crashes on LOAD → infinite
spawn/INIT/LOAD/crash loop.

### A9. MAJOR · ⚠️ — Runtime `error` handler tears down the wrong client
`_teardown` from `proc.on('error')` runs without the `this.proc === proc` identity check the
`exit` path uses (`wallpaperRuntime.cjs:50-53,64-72`): a late `error` from a stale process
nulls `initPromise` and rejects the *live* process's pending requests → re-`INIT` of an
already-initialized engine (the exact thing the init guard claims to prevent).

### A10. MINOR · ⚠️ — Sync unbounded logging + oversized-frame desync
`logLine` does `appendFileSync` on the main thread for every stderr chunk
(`main.cjs:37-41`, `wallpaperRuntime.cjs:45-48`), file never rotated; and after an oversized
frame the parser drops the whole buffer with no resync (`wallpaperRuntime.cjs:76-82`), after
which every command times out.

---

## Module-platform / `rex-host` bugs (not on the wallpaper-runtime path)

### B1. CRITICAL · V — `rex.settings` dereferences a null service → access violation
`ServiceContainer::findService` returns `nullptr` (no throw), `IModuleContext::getService<T>`
returns that as a null `shared_ptr` (`IModuleContext.hpp:24-28`), and `SettingsModule`
dereferences it: `*bus_`, `*configStore_` (`SettingsModule.cpp:22-23,42`). Nothing seeds
EventBus/ConfigurationStore into the container (the only `registerService` calls register each
module's *own* service). Null deref → AV, which `catch (const std::exception&)` cannot catch.
### B2. CRITICAL · ⚠️ — Shutdown rewrites `modules.json` with empty settings → settings wiped every exit
`ModuleOrchestrator::shutdown` rebuilds configs without `settingsJson`
(`ModuleOrchestrator.cpp:74-83`), deleting everything `SettingsService::persistSetting` wrote.
### B3. CRITICAL · ⚠️ — `std::stoi` on the profile auto-switch thread → `std::terminate`
`ProfileManager.cpp:182` converts a settings string with no try/catch anywhere in the file
(verified: no `try`/`catch` in the file). A non-numeric `taskbar.position` kills the whole
host. Same loop `unlock()`s a non-owning lock after `break` (UB) when 2+ profiles match
(`evaluateTrigger` returns true for every non-Manual trigger).
### B4. CRITICAL · ⚠️ — `FreeLibrary` while module objects/threads still run its code
`reloadModule` drops the registry's `shared_ptr` and unloads immediately
(`ModuleOrchestrator.cpp:154-162`); never calls `shutdown()`, never re-adds to `startupOrder_`.
Compounded by detached command workers (B5).
### B5. CRITICAL · ⚠️ — `CommandExecutor` detaches a worker that captures `this` with no join/dtor
(`CommandExecutor.cpp:30-42`, `CommandExecutor.hpp:13-27`) — the executor can be destroyed
(module stop/unload) while the worker locks `mutex_` and runs `def.execute`.
### B6. MAJOR · ⚠️ — Shortcut callback + EventBus publish invoked while `ShortcutManager::mutex_` is held
→ any command that inspects/changes shortcuts self-deadlocks (`ShortcutManager.cpp:19-24,56-65`).
### B7. MAJOR · ⚠️ — `ServiceContainer` has no mutex at all (unlike every neighbouring registry)
(`ServiceContainer.hpp:11-37`) — becomes a live race the moment B1 is fixed.
### B8. MAJOR · ⚠️ — `ConfigurationStore::saveModule` is an unsynchronized read-modify-write with
non-atomic in-place truncate (`ConfigurationStore.cpp:218-238`, `:213`) → lost settings on
concurrent writes; crash mid-write looks like a fresh install.

---

## Minor / latent (agent-reported)

- ⚠️ `D3D12Renderer::Resize` discards `SwapChain::Resize`'s result and never rebuilds the HDR PSO when the back-buffer format flips (`D3D12Renderer.cpp:1250-1254`) → stale `m_isHdrOutput`, RTV format mismatch after moving a monitor onto an HDR output.
- ⚠️ `UploadTextureToGPU` placed-footprint `RowPitch` unaligned for widths not multiple of 64 (1366/1440/1680/3440) (`D3D12Renderer.cpp:1348` vs `WICTextureLoader.cpp:118`).
- ⚠️ `Shutdown()` never clears `m_sharedQueue`; `SwapChain::Shutdown` early-return leaks the frame-latency HANDLE; `GetRTVHandle` unbounded with fixed `[3]` arrays.
- ⚠️ `Compositor::ImportLayerTexture` re-import leaks an SRV slot per resize (`compositor.cpp:539-553`) → eventually "out of SRV slots".
- ⚠️ `Engine::LoadScene` increments `m_scenesLoaded` before the failure path returns (`Engine.cpp:357,408`) → telemetry drifts.
- ⚠️ `VideoManager::Initialize` publishes the borrowed device pointer before validating and short-circuits when already initialized (`VideoManager.cpp:17-38`).
- ⚠️ `ModuleOrchestrator::update` overwrites `ModuleHealth` with a fresh struct each frame, wiping crash/restart counts — so the 3-restart limit can never engage (`ModuleOrchestrator.cpp:90-94,171-182`).
- ⚠️ `ProfileManager` never serializes `ProfileTrigger`, so `autoSwitchLoop` can never fire a trigger; wallpaperId/themeId are stored but never applied (`ProfileManager.cpp:9-24,171-211`).
- ⚠️ Dead code: `app/electron/supervisor/*`, `wallpaper-library.cjs`, `app-state-manager.cjs` never required; `src/hooks/useAppState.jsx` calls a preload bridge that doesn't exist.

---

## Refuted (checked, not bugs)

- ⚠️→**refuted** "runtime process survives app shutdown / logoff and orphans the desktop layer":
  `ReadExact` returns false on a 0-byte pipe read (`wallpaper_cli_main.cpp:473-474`), so
  stdin EOF from the dying Electron process cleanly unwinds `RunRuntime` → engine shutdown.
  Only a *stuck* runtime (e.g. A2's spin inside Explorer calls) could survive.
- **refuted** "frame parser corrupts on chunk boundaries": `_onData` handles multi-frame
  chunks, split frames and bounded growth correctly; only the oversized-*recovery* path (A10) is broken.
- **refuted** "ipcMain handlers stack / double registration": all registrations happen once
  inside `app.whenReady()`; the one constructor-time registration is in never-required dead code.

---

## Suggested fix order

1. **A2** (spin/brick — worst user impact, tiny diff: clear `m_deviceRemoved`, pace retry, re-arm flag)
2. **A1** (UAF on every recovery — needs a locking/snapshot decision, ~half day)
3. **A7** (one-line packaging fix: add `assets/**` to `files`)
4. **A5** (one-line: move `Play()` into the `PLAY` branch)
5. **A3 + A4** (renderer correctness; A3 likely overlaps the HDR present-bug investigation)
6. **A6, A9, B1, B3** (races/crashes; B1 needs EventBus/ConfigurationStore seeding)
7. Everything else as a backlog sweep.
