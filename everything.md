# REX — Everything

> One-file reference: what REX is, what is in the repo, **what is already DONE**, what is WIP, and where the docs live.
> Last updated: **2026-10-07**
> Detailed per-feature status: [`#project_status.md`](#project_status.md) · Remaining work: [`TODO.md`](TODO.md)

---

## What REX is

A modern desktop personalization platform for Windows 10/11 — the layer between Windows and the user that Windows never built:

- **Widgets** — live info on the desktop (clock, weather, system monitor)
- **Wallpaper** — image, video, web, shader, scene, audio-reactive
- **WindowFX / Personalization** — taskbar, explorer, start menu, blur/acrylic/mica
- **Automation** — event-driven workflows (planned)
- **SDK + Marketplace** — build/share widgets and content (planned)

Competitor space: Rainmeter + Wallpaper Engine + StartAllBack + Seelen UI, unified in one product.

---

## Scoreboard (from `#project_status.md`, audit 2026-07-25)

| State | Count |
|---|---|
| ✅ Done | ~121 |
| 🟡 Partial/stub | ~24 |
| ⬜ Not started | ~164 |

> ⚠️ The status file predates the big UI rewrite. Since the audit, the native C++ shell UI was retired into `dump/` (102 files) and the current UI is the **Electron + React app** (`app/`) — see [UI story](#ui-story-three-generations).

---

## Repo map

| Path | What it is |
|---|---|
| `rex-core/` | **C++20 native core** — wallpaper engine, graphics (D3D12), IPC, modules, platform (WorkerW), profiles, desktop_personalization. vcpkg deps, gtest/gmock, tools, plugins |
| `rex-shared/` | Shared C++ library (headers/src, own build) |
| `rex-shell/` | **C# solution** — REX.Shell (XAML), .Services, .Tray, .CLI, .Tests (legacy/parallel shell track) |
| `app/` | **Current UI: Electron + React 19 + Vite + TS/JSX** — wallpaper gallery/playlist, widgets workspace, inspector, mica backdrop; packages `rex-wallpaper-runtime.exe` + ffmpeg DLLs into a Windows installer (`dist:win`) |
| `src/ui/surface/` | **REX UI framework — Phase 1: Surface System** (TypeScript): Surface core + material implementations + design tokens |
| `proto/` | Protobuf schemas for IPC (named pipes) |
| `docs/` | RES spec suite, ADRs, developer guide, design language, cloud-sync spec |
| `dump/` | Retired native C++ shell UI (24-component library, pages, LiquidGlass renderer) — 102 files archived |
| `bench-media/` | Media decode/render benchmarks |
| `sessions/` | Dated working-session notes |
| `#project_status.md` | Feature-by-feature status audit (the done/todo source of truth) |
| `phases.md` | Wallpaper-engine development phases (1–10) |
| `fix.md`, `LS.md`, `Modify.md`, `r.md` | Large working notes (debugging/repack/modify logs) |
| `hdr_*.log`, `repack*.log` | HDR present-bug and repack investigation logs |
| `.opencode/` | opencode config: GSD install, superpowers plugin, MCP servers |
| `AGENTS.md` | **Standing rules** — main job = fix bugs (app + engine); update `TODO.md`/`everything.md` after every finished work unit |

---

## Architecture & stack

```
Core (C++20)  ←── IPC: named pipes + protobuf ──→  Shell/UI
  • WorkerW host (behind desktop icons)             • Electron + React (current)
  • D3D12 compositor + render thread                • C# WinUI/XAML (rex-shell, parallel)
  • Wallpaper renderers (6 kinds)                   • TS Surface System (design framework)
  • Module system (load/lifecycle/health)
  • Desktop personalization adapters (registry)
```

- **Language decision:** C++20 core now, Rust deferred post-Phase 3 (ADR-006)
- **Video:** Media Foundation decoder **+ ffmpeg pipeline** (avcodec-62/avformat-62 packaged with app)
- **Build:** CMake + vcpkg (rex-core), dotnet (rex-shell), pnpm + Vite (root `package.json` → `app/`), Electron packaging
- **Tests:** gtest/gmock + ctest (C++), dotnet test (C#), oxlint + tsc (TS)
- **Logging:** spdlog/fmt (DLLs shipped in installer)

---

## DONE already ✅

### Wallpaper engine (the most mature area)

- **6 renderers:** Image, Video (MF + ffmpeg), Web, Shader (D3D12 root sig + PSO + HLSL), Scene (ECS + RenderPipeline), Audio visualizer (WASAPI + FFT)
- **Decoders/loaders:** MediaFoundationDecoder, WICTextureLoader, ImageLoader, SceneLoader
- **Playback:** play/pause/stop, seek, loop, speed, brightness/contrast/saturation, volume, audio-reactive, fill mode
- **Telemetry:** frames decoded/dropped, FPS, latency (`PlaybackTelemetry`)
- **Multi-monitor:** independent per-monitor assignment, display-change detection
- **Engine:** LoadScene/UnloadScene, background render thread, D3D12 compositor, recovery (`ReinitializeCompositor`)
- **Shader pipeline:** D3D12 root signature + PSO + HLSL compilation
- **Recent (git):** ffmpeg video pipeline, D3D12 renderer work, shutdown-crash fixes (`4ba29fa`, `1ca6195`)

### Widgets

- **WidgetHost** — transparent layered HWNDs (WS_EX_LAYERED/TOPMOST/NOACTIVATE/TOOLWINDOW)
- **3 built-in widgets:** Clock, Weather (Open-Meteo, 30-min refresh), System Monitor (CPU/GPU/RAM/NET)
- Opacity control; blur-behind (partial); JS bridge skeleton (close + getStats handlers)

### Desktop personalization (StartAllBack-class features)

- **Registry adapters:** WinTaskbarAdapter (`TaskbarAl`, `TaskbarAutoHide`, `TaskbarSmallIcons`), WinExplorerAdapter, WinStartMenuAdapter
- **Services:** PersonalizationService (orchestrator), Taskbar/Explorer/Theme/Appearance/Effects/Desktop managers, ProfileManager, SettingsManager
- Taskbar position/icon-size/auto-hide ✅ · Explorer acrylic/mica toggles ✅ · Start menu style dropdown (Win11/Win10/Classic) ✅
- Module wiring: `rex.personalization` registers `IPersonalizationService`

### Profiles & backup

- ProfileManager: list/CRUD, import/export, **rollbackToLastGood**

### Theme system

- ThemeRegistry, ThemeService, TokenManager, StyleResolver, ComponentRegistry

### Module platform (plugin infrastructure)

- ModuleRegistry, ModuleManager, ModuleLoader (built-in factory + dynamic DLL), ModuleOrchestrator (deps + startup order + health)
- LifecycleCoordinator (initialize/start/stop/shutdown in dependency order), HealthMonitor (crash/restart counts)
- ConfigurationStore (JSON), InProcessSandbox, SignatureVerifier, DependencyResolver (topological sort + cycle detection), ServiceContainer, EventBus
- Plugin SDK: `IModule` / `IModuleContext` lifecycle + events; Package manager: DependencyResolver ✅

### UI — three generations (see below)

- Native C++ shell: 24-component library + 7 pages + shell infra — **built, then retired to `dump/`**
- **Electron app (current):** wallpaper gallery/playlist/apply, media controls, widgets workspace (collection/details/preview), desktop preview, inspector panels, mica backdrop, sidebar, Shadcn-style gallery, packaged Windows installer
- **Surface System (Phase 1):** `src/ui/surface` — Surface core (Surface, Context, Factory, Provider, Registry, useSurface), **6 materials** (Acrylic, Liquid Glass, Mica, Glass, Solid, Transparent), **design tokens** (colors, spacing, radius, shadow, opacity, blur, border, animation)

### Docs & specs (all written)

- **RES suite (16 docs)** in `docs/res/`: index, widget/desktop/SDK-marketplace/wallpaper/WindowFX/automation/rendering/testing platforms, API reference, Phase 0–1 implementation, ADR-001–006
- `docs/REX-Liquid-Glass-Design-Language.md`, `docs/REX-Cloud-Sync-Spec.md`, `docs/wallpaper-engine-architecture.md`
- Developer guide: getting-started, coding-conventions, ADR process, testing guide
- `README.md` roadmap + architecture; `phases.md` wallpaper phase plan

### Tooling & diagnostics

- gtest/gmock vendored (vcpkg), ctest, dotnet tests, oxlint
- `bench-media/` media benchmarks; HDR present-bug harness (`hdrprobe.obj`, `present_repro.obj`, 10+ diagnostic logs)
- E2E screenshot pairs (`e2e_a/b.png`, `wall_a/b.png`) for visual comparison
- **opencode AI tooling (installed 2026-10-06):** superpowers plugin, GSD (spec-driven workflow, 18 commands), Context7 + Vercel Grep MCP
- **BUG-001 investigation closed (2026-10-06):** root-cause report + ordered fix plan for "wallpaper jumps above icons / input hang" — [`docs/bugs/BUG-001-workerw-zorder-input-hang.md`](docs/bugs/BUG-001-workerw-zorder-input-hang.md); fixes pending (see `TODO.md` 🐛)
- **Parallel subsystem audit (2026-10-06):** 4 read-only agents (engine lifecycle / D3D12 / Electron main / module platform) + hand-verification of all high-severity claims — [`docs/bugs/AUDIT-2026-10-06-parallel-audit.md`](docs/bugs/AUDIT-2026-10-06-parallel-audit.md): ~10 shipped-path bugs (2 critical: device-loss hot-spin, telemetry use-after-free), 8 module-platform bugs, minors; 3 claims refuted with evidence
- **Audit fixes shipped (2026-10-06, A1–A5 + A7 + A9):** device-loss hot-spin/brick fixed with a paced, re-armed, backoff-capped recovery path (`RecoveryBackoff.hpp` + 4 unit tests) and loss-flag reset in `D3D12Renderer::Shutdown()`; telemetry/command use-after-free closed by locking `m_mutex` across the recovery swap and in the unlocked accessors; SDR-on-HDR normalization restored to paper white; missing `PIXEL_SHADER_RESOURCE→COPY_DEST` transition added; `Play()` no longer runs before PAUSE/STOP/UNLOAD; packaged app ships the tray icon again; stale-process `_teardown` no longer clobbers the live client. Verified by native build + targeted gtest run + repackaged-asar diff (icon MISSING before → PRESENT after) + oxlint
- **Color-accuracy fixes shipped (2026-10-07, A11–A13):** HDR10 paper-white norm (`norm = max(adj.w, 1.0)`), pre-ACES clamp lifted for HDR input, `SrgbToLinear` restored in the SDR decode paths, broken-ffmpeg-reference comment replaced with the ITU-R BT.2408 justification. Verified three ways: exact HLSL port predicts ITU targets (203 nits → 232/255 ≈ 91%, highlights 397–1004 nits keep gradation 245–253; SDR path exact identity); clean MSBuild with no warnings; **real GPU end-to-end** — rebuilt runtime rendered the 8-band HDR video (9–1004 nits) on screen and all bands matched prediction within ±4 codes, zero shader/present errors, real LG OLED 4K HDR file decodes on `HW_P010` with back buffer confirmed SDR. Build `06CF22093D59C06B` (uncommitted, needs `app/dist-exe` repack)
- **Color-accuracy root cause (2026-10-07):** the "HDR colors are wrong" complaint is now explained by three concrete defects in `g_pixelShaderSource`, each verified numerically rather than by eye — (1) `norm = masterMax` puts 203 nits at 150/255 instead of BT.2408's ~90%; (2) a pre-ACES `saturate()` collapses all ≥203-nit values to one code, killing highlight gradation; (3) the SDR path double gamma-encodes (128→188) because `SrgbToLinear` became dead code in the 10-day recovery rewrite, while A3's paper-white fix was reverted at `:245`. Method: exact Python port of the HLSL (`shader_port.py`) + controlled test videos with known inputs (`hdr_bands.mp4` at 9.1/40/98/204/397/594/806/1004 nits, produced via `zscale … npl=10000` after discovering the default `npl=100` silently encoded 100× too dim) + DPI-aware screen sampling. **One prior claim refuted:** static images are drawn by Windows (`SystemParametersInfoW`), not by the shader, so the earlier "static = exact passthrough" result proves nothing about the shader. Fixes pending → `TODO.md` A11–A13
- **BUG-001 P0-A + P0-B shipped (2026-10-06):** wallpaper layer repair is non-destructive — a mispositioned layer is moved back below the icons (`TryReanchorLayer`, policy extracted to `platform/windows/LayerRepairPolicy.hpp`, 7 unit tests) instead of being destroyed, rebuilt and reloaded, and the `WM_SPAWNWORKERW` storm only fires when a window is actually gone; the synthetic `REXSyntheticWorkerW` is now `WS_EX_NOACTIVATE | WS_EX_TRANSPARENT` with a verify invariant, so desktop clicks pass through even while it sits above the icons. Verified: `rex-core-tests` 7/7 + 8/8 desktop-host tests, runtime builds. Remaining: P1-D instrumentation
- **BUG-001 P0-C shipped (2026-10-07):** `DesktopWindowThread` is finally wired up — it was dead code, absent even from `CMakeLists.txt`. Every mutating HWND operation (WorkerW discovery, create+reparent, destroy, synthetic-layer destroy, re-anchor) now runs on that pumped thread, so Explorer's cross-process messages can no longer queue onto a pump-less command/monitor/render thread (AppHangXProcB1 shell freezes). Verified by driving the real runtime binary through INIT → 3 s monitor verify loop → SHUTDOWN with the window thread starting and exiting cleanly; suites: 34/34 core, 11/11 module, 7/7 policy
- **Module-platform crash fixes (2026-10-07):** `rex.settings` no longer dereferences a null service (`ModuleOrchestrator::initialize` publishes `EventBus` + `ConfigurationStore` into the `ServiceContainer`; the module fails loudly instead of crashing), and the profile auto-switch thread can no longer `std::terminate` the host on a bad `taskbar.position` value (validated parse + exception-guarded applies + the double-`unlock()` UB removed)
- **Fullscreen-pause false positive fixed (2026-10-07):** the wallpaper froze ~2 s after every Play whenever a maximized window was open — the auto-pause treated any window covering >=95% of the monitor as fullscreen, and a maximized window with a taskbar covers 97.8%. Now requires (near-)full-monitor coverage and ignores shell chrome, with the triggering window logged. Verified live against the 4K60 VP9 file: position advanced at exactly 1.00x for 20 s with decode P50 ~0.65 ms
- **Pause-flash debounce + position continuity (2026-10-07):** a full-length playback test caught a ~1 s `XamlWindow` overlay pausing/resuming the wallpaper and rewinding the reported position (20 s → 6 s) while the video itself continued. The pause now requires 1.5 s of persistent fullscreen, and pause/resume folds elapsed time into a position base so the clock continues instead of rewinding (the decoder keeps its own PTS; a wrong position would also mis-seek the next device-loss recovery). Proven live: pause froze pos at 6.06, resume continued to 10.08
- **Runtime deep-dive audit (2026-10-07, research only, no code changes):** 5 parallel audits (command protocol / decode pipeline / playback integration / present+pacing / completeness gaps) with every high-severity claim hand-verified — [`docs/bugs/AUDIT-runtime-deep-dive.md`](docs/bugs/AUDIT-runtime-deep-dive.md): 20 findings R1–R20 (critical: hot-reload self-deadlock inside Begin/End, software-decode RGB-into-YUV corruption + per-frame pool rebuild, multichannel audio ring overflow; high: static full-rate presents, 0-frame loop hang, Stop splitting A/V, dropped volume/mute, stale seek frames, hostile-path crash, zero-length=EOF kill, unchecked DuplicateHandle, blocking-close hang, decoder Seek/Close race, snapshot-once HDR, unfenced allocator reset) plus 8 completeness gaps G1–G8 (no power/session-lock awareness, topology holes, endpoint loss, no protocol versioning or second-runtime guard, no resource bounds, no user error surface, unbounded native logs). All recorded in `TODO.md`, none implemented yet
- **Module-platform batch 2 + Electron + engine races (2026-10-07):** shutdown preserves `settingsJson`/`restartCount` instead of wiping them; `reloadModule` shuts the module down, destroys it before `FreeLibrary`, serializes against all snapshot paths and keeps `startupOrder_`; `CommandExecutor` joins its workers (no more detach) and cancelled commands stop reporting success; shortcut events/callbacks fire outside the lock; `ServiceContainer` is mutex-guarded; config writes are atomic (temp + `MoveFileExA` replace) and race-free — the existing settings round-trip test caught my own re-lock bug in `loadModule` during this work. Render loop works on locked snapshots of scene/capture state (no more torn reads or freed capture device). Crash recovery is re-entrancy-guarded and capped at 3 failed rounds; the app log rotates at 2 MB; a corrupt frame stream restarts the runtime instead of failing every command. ProgmanHost got the same window-thread ownership treatment (plus a GDI-leak and a `PostQuitMessage` fix). Suites: 34/34 core, 11/11 module, 82/82 command, 40/40 settings, 7/7 layer policy, 4/4 backoff
- **Native-engine baseline adopted (2026-10-07 13:50):** the runtime deep-dive audit is now the implementation baseline — correctness before optimization or new wallpaper types. Order: P0 WorkerW/recovery crash protection (BUG-001 must survive reconstruction + R13 stale-client detach), HDR state/color pipeline (R16+G3, OS state not monitor capability), SW-decoder pool corruption + per-frame rebuild (R3+R4, HW=P010/NV12, SW SDR=BGRA, SW HDR=RGBA16, one-time pool creation), D3D12 allocator fencing (R17, FrameContext ring); P1 FrameScheduler::Clear on seek/loop/recovery (R10), Stop resets full A/V timeline (R8), one-path loop+speed+volume+mute restore (R9), Seek/Close single ownership (R15), AVIOInterruptCB (R14), bounded EOF + backoff (R5), audio ring clamp (R6), latency-flag preserve (R7), PTS-based position (R20); P2 static render-once (R2), cadence=min(maxFps, decoder) (R18), hot-reload/spectrum outside gate (R19), protocol hardening (R11+R12+R13); P3 power/session-lock/topology/endpoint/mutex+versioning (G1-G5, plus G6/G7/G8); P4 benchmarks only after P0-P3. Recorded as P0-1..P4 checklist in `TODO.md`. GPU/memory numbers are meaningless until the correctness bugs are fixed
- **P0-1 shipped (2026-10-07 14:32):** WorkerW/desktop-layer crash protection + R13 stale-client detach, TDD (RED witnessed: C3861/C1083 + a failing liveness-rights test). (1) Reconstruction survival: `ReacquireWorkerW` now destroys the previous synthetic layer and resets `m_syntheticWorkerW` via tested `PlanSyntheticTransition` (`LayerRepairPolicy.hpp`, `SyntheticLayerTransitionTest` 2/2) — a stale `true` across synthetic→real used to fail verification forever. (2) R13: new `wallpaper/runtime/ClientSurface.hpp` (`IsProcessAlive` via `GetExitCodeProcess` so it works with the binding's query-only rights, `ValidateDuplicatedSurface`, `DetachClient`) wired into `wallpaper_cli_main.cpp`: checked `DuplicateHandle` + ok:false, dead-client detach gate, new `DETACH_CLIENT` command, detach (not just close) on SHUTDOWN/stdin-EOF. `ClientSurface` suite 10/10. Proven live: dead pid → ok:false "client process exited (surface detached)", live self-bind → 8 slots + real fence, DETACH → self-preview resumes, SHUTDOWN clean exit 0. Suites: 36/36 core, 73 wallpaper (+2 env-skipped; `EngineTest.VideoScenePlayback` needs a `test.mp4` fixture in CWD — filed as test-gap). **New verified finding:** a second concurrent runtime's startup/shutdown silently kills the other runtime (no log, no dump) — filed in `TODO.md`, fix is the P3 named-mutex (G5)
- **P0-2 shipped (2026-10-07 15:08):** HDR state/color pipeline, TDD (RED: C1083; 11 new tests green). `SetForceSdr` before `Initialize` in both engine paths; `SwapChain::RefreshHdrSelection` + `D3D12Renderer::RefreshHdrOutput` (tested `PlanHdrSync`) close the R16 snapshot-once gap: flag refresh + lazy HDR-PSO creation on every `Resize` and every `CheckDisplayChanges` pass (same-res OS toggles included, idempotent when steady). Topology predicate widened to geometry+id+primary+DPI (tested `MonitorTopologyChanged`, 6/6); real per-monitor DPI via `GetDpiForMonitor` with `Shcore.lib` on the 3 Engine.cpp targets. Proven live on the 4K HDR file (1.00x, clean shutdown) + --force-sdr smoke; no render-mutex deadlock (refresh runs unlocked on the render thread like `Resize`). Suites: 36/36 core, 84 wallpaper (+2 env-skipped). Limitation: no live SDR->HDR toggle test possible (OS HDR off on this box)

---

## In progress / WIP 🟡

| Item | Notes |
|---|---|
| **Color accuracy (HDR dark / SDR washed out)** | **Fixed + verified on GPU 2026-10-07** (A11–A13) — build `06CF22093D59C06B`, uncommitted, needs `app/dist-exe` repack + eyeball A/B |
| Color-verification harness | Lives in `%TEMP%\opencode\rex-verify\` (shader port, band generators, IPC client, DPI-aware sampler) — **not committed**, will be lost on temp cleanup |
| Uncommitted core changes | `D3D12Renderer.cpp`, `WorkerWManager.cpp/.hpp` modified, not committed (recovery point commit `1ca6195` exists) |
| HDR present bug | Long investigation: `hdr_*.log` (14 files), `hdrprobe.obj`, `present_repro.obj`, notes in `fix.md` |
| ffmpeg video pipeline | Landed in git; GPU-native plan docs in `461b79c`; packaging wired into Electron build |
| Widget WebView2 JS bridge | `HandleWebMessage` stubbed — MSWRL::Callback issue blocks full init |
| FPS counter | Hardcoded "60" in inspector |
| Marketplace | `CurlMarketplaceClient.cpp` basic HTTP client only |
| UpdateManager | Stub interface |
| Win32PlatformAdapter | Exists; real Win32 hooking TBD |
| Transparency/labels/replace toggles | Wired in UI, backend partial |

---

## UI story (three generations)

1. **Native C++ shell** — full 24-primitive component library + pages (Wallpaper/Widgets/Personalization/Home/…) + LiquidGlass renderer → **retired to `dump/` (102 files)**
2. **C# XAML** (`rex-shell/`) — Shell/Tray/CLI/Services/Tests solution — parallel/legacy track
3. **Electron + React** (`app/`) — **current direction**; root `package.json` ("rex-ui", Phase 1 Surface System) is the new design-system layer in TS

---

## Docs index

| Doc | Covers |
|---|---|
| `README.md` | Overview, architecture, roadmap (phases 0–6) |
| `#project_status.md` | Per-feature ✅/🟡/⬜ audit with file paths |
| `phases.md` | Wallpaper engine phases 1–10 |
| `docs/res/00_INDEX.md` | Full spec index + reading order |
| `docs/res/ADR-001-to-006.md` | Stack, IPC, sandbox, persistence, telemetry, language |
| `docs/developer-guide/*` | Getting started, conventions, ADR process, testing |
| `everything.md` | This file |
| `TODO.md` | Everything remaining, prioritized |

---

## Build & run

```powershell
# Native core
cmake -S rex-core -B rex-core/build
cmake --build rex-core/build
cd rex-core && ctest

# C# shell
dotnet build rex-shell/REX.sln
dotnet test rex-shell

# Electron app
pnpm install
pnpm dev              # vite
pnpm build            # vite build
pnpm electron         # build + run electron
pnpm dist:win         # package Windows installer (ships rex-wallpaper-runtime.exe + ffmpeg DLLs)

# Typecheck / lint (root)
pnpm typecheck
pnpm lint
```

---

## Notes & caveats

- **Standing workflow (AGENTS.md, 2026-10-06):** main job = fix bugs in `app/` + `rex-core`; every finished work unit must be checked off in `TODO.md` and added to the ✅ inventory here.

- `#project_status.md` is an audit from **2026-07-25** — still authoritative for the engine/modules, but its "Shell UI" section describes code now sitting in `dump/`.
- README previously claimed "Pre-development (Phase 0)" — outdated; the engine and app are well past that.
- GSD lives in `.opencode/` (local install): run `/gsd-new-project` after restart to start spec-driven phases.
