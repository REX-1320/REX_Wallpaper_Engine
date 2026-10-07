# REX — TODO

> Everything remaining, prioritized. Checked items move to [`everything.md`](everything.md) / `#project_status.md`.
> Last updated: **2026-10-07** · Source of truth for per-feature status: `#project_status.md` (~164 ⬜, ~24 🟡)

---

## 🐛 Bug backlog (top priority — main job)

> Full audit with file:line evidence + refuted claims: [`docs/bugs/AUDIT-2026-10-06-parallel-audit.md`](docs/bugs/AUDIT-2026-10-06-parallel-audit.md) (2026-10-06, 4 parallel subsystem audits, high-severity items hand-verified).

### New discoveries (2026-10-07, from P0-1 verification)

- [ ] **G5-interference · MAJOR** — a second runtime startup/shutdown silently kills the other runtime: Debug runtime died with no log/crash-dump within seconds of the Release runtime starting (14:20:43) and again overlapping its shutdown (14:24:23); alone, the full INIT/LOAD/PLAY/client-surface/DETACH/SHUTDOWN sequence passes repeatedly. Exact kill mechanism unknown (shared WorkerW layer suspected). Fix per baseline P3: native named-mutex ownership + protocol versioning (`docs/bugs/AUDIT-runtime-deep-dive.md` G5)
- [ ] **Test-gap · MINOR** — `EngineTest.VideoScenePlayback` fails when CWD has no `test.mp4`: `LoadScene` now (correctly, per prior fail-open hardening) returns an error for missing files, but the test still expects success for a nonexistent path. Passes with a real file present. Fix: stage a fixture or expect failure for missing files
- [ ] **SW loop-boundary stall, intermittent (2026-10-07, 1/5 runs)** — SW SDR playback froze once at exactly 33 frames (first EOS) with no error/seek log, responsive process, clean SHUTDOWN; 4 subsequent identical runs looped cleanly (258+ frames). Rigorously exonerated once via stash A/B on a different symptom pattern — but this single stall is unexplained. Suspects: EOS/loop-seek race (P1-10 family) or second-runtime interference (Release runtime active). Needs a dedicated repro work unit if seen again
- [ ] **SW AV1-HDR decode fails at send_packet -40 (pre-existing, blocks SW-HDR e2e)** — 4K AV1 HDR file with hardwareAcceleration:false decodes zero frames; `avcodec_send_packet failed (-40)` spams the log unboundedly (tight loop, no backoff — P1-10 family) while the engine clock advances on silence. Upstream of the pool code (no frame ever reaches EnsurePool). Fixing unlocks end-to-end RGBA16-pool verification
- [ ] **TDR under dual-4K load (observation 2026-10-07)** — HW 4K60 session alongside the Release wallpaper's 4K session died with repeated DXGI_ERROR_DEVICE_REMOVED + recovery re-inits; the same session loops cleanly alone. No action (P4 perf context), but dual-HW-session load is a known cliff
- [ ] **Playback slow-motion + stutter on AV1 4K60 (2026-10-07, diagnosed, fix planned)** — `blk4k.mp4` (AV1 3840x2160 59.94fps, AAC) plays at **0.55x with judder**: decFps/rendFps lock at ~33 while the source is 59.94. Evidence, not guessing: `r_frame_rate`/`avg_frame_rate` are correct 60000/1001 and `m_frameIntervalMs` is the correct 16.68 ms (`VideoPlaybackController::Open :41-42`); per-frame CPU is ~1.1 ms (proc 48.6 s / ~42k frames) so it is NOT software-bound; fixed-function `Video Decode 1` engine sits at 0% while AMD VCN `Video Codec 0` runs 51–60% — the fixed-function AV1 path is the throughput ceiling for this content on the RX 6700 XT. Architecture fault (user chose fix direction): the decoder is the playback clock — `DecoderThread` paces to source rate, `FrameScheduler::Tick` ignores `targetPts` and returns latest, the render loop presents only fresh frames — so a decoder shortfall slows **time** instead of dropping frames. Fix = real-time wall clock + PTS-aware latest-frame selection + stale-frame drops + threaded SW decode (`thread_count = 1` at `FfmpegVideoDecoder.cpp:146,393,411` throttles the SW fallback: system-ffmpeg SW-1thread does 22.8 fps vs 61.9 threaded on the same file). Aggravating stutter sources found live: two stray `bench-media\mpeg4_320x240.mp4` runtimes (17:55, 18:16, ringBytes 2457600 = 320x240x8 slots) and a brief double-spawn from the soak harness itself, all presenting to the same WorkerW — second-runtime guard still missing (G5). Plan: `docs/superpowers/plans/2026-10-07-realtime-playback-clock.md`. Soak data: 34-min HDR run, CPU 3.1% avg, GPU-3D 9% avg, VideoCodec ~55%, priv 377→416 MB (+38 MB — watch item), IO 2.5 MB/s, 68387 decoded / 455 dropped / 67931 presented. NOT captured: GPU clock/power (ADL Overdrive5 dead rc=-1, HWiNFO blocked, LibreHardwareMonitor v0.9.6 located but not yet wired), P95/P99 frame times (harness didn't sample `decodeMsP50/95/99`), startup latency (runtime.log rotated by stray instances)

### Engine / runtime (shipped path)

- [x] **A2 · CRITICAL** — device-loss recovery hot-spin / brick — **fixed 2026-10-06**: one gated recovery path with mandatory pacing, re-armed flag + capped backoff (`Engine.cpp` RenderLoop, new `wallpaper/engine/RecoveryBackoff.hpp`, 4 unit tests), and `m_deviceRemoved` cleared in `D3D12Renderer::Shutdown()`. Verified: compile+link green, `RecoveryBackoffTest` 4/4 pass
- [x] **A1 · CRITICAL** — telemetry/command use-after-free during recovery — **fixed 2026-10-06**: `m_mutex` now held across the renderer/video teardown+rebuild in `ReinitializeRenderer`, and taken by the previously unlocked `GetTelemetry`, `GetDuration`, `SetPerMonitorWallpaper`, `SetSettingsService`. Verified: compile+link green
- [x] **A3 · MAJOR** — SDR-on-HDR brightness regression from the uncommitted WIP — **fixed 2026-10-06**: non-HDR branch normalized by paper white again (`rgb * adj.w`); mastering-max divisor kept only for PQ/HLG input. `SetPaperWhiteNits` is live again. Needs an HDR-monitor visual check (may be related to the HDR present bug below)
- [x] **A4 · MAJOR** — `CopyTextureRegion` into a texture left in `PIXEL_SHADER_RESOURCE` — **fixed 2026-10-06**: `PIXEL_SHADER_RESOURCE → COPY_DEST` transition emitted before the copy when the texture was not just recreated
- [x] **A5 · MAJOR** — `PAUSE`/`STOP`/`UNLOAD` called `Play()` first — **fixed 2026-10-06**: only the `PLAY` branch calls `Play()` (`wallpaper_cli_main.cpp`)
- [x] **A7 · MAJOR** — packaged build had no tray icon (unreachable app) — **fixed 2026-10-06**: `assets/**` added to electron-builder `files`. Proven both ways: asar MISSING the ico before, PRESENT after
- [x] **A9 · MAJOR** — runtime `error` tore down the live client — **fixed 2026-10-06**: `_teardown` returns early for a stale process (`wallpaperRuntime.cjs:64`). Verified: `node --check`, oxlint clean
- [x] **A6 · MAJOR** — render-thread state races — **fixed 2026-10-07**: the render loop snapshots scene type, hot-reload path+timestamp and the audio-capture device under a short m_mutex and works on the copies (m_audioCapture unique_ptr → shared_ptr); HandleDeviceLoss copies the scene under the lock; the hot-reload timestamp is published back under the lock. Verified: runtime builds, targeted suites green
- [x] **A8 · MAJOR** — crash-recovery respawn storm — **fixed 2026-10-07**: recoverRuntime() is re-entrancy-guarded and capped at 3 consecutive failed rounds, then parks until a new wallpaper is applied (which resets the counter); also guarded the SET_FILL_MODE send. Verified: node --check, oxlint clean
- [x] **A10 · MINOR** — unbounded sync logging + frame-stream desync — **fixed 2026-10-07**: rex-app.log rotates aside at 2 MB; a corrupt/oversized frame kills the wedged process so crash-recovery replays on a fresh stream (bounded by the A8 round cap). Verified: 6/6 behavioral checks pass, oxlint clean
- [x] **A14 · MAJOR** — wallpaper froze ~2 s after every Play behind any maximized window — **fixed 2026-10-07**: the fullscreen auto-pause used a >=95% monitor-area test, but a maximized window with a taskbar covers 97.8%. Now requires (near-)full-monitor coverage (2 px tolerance for borderless windows) and ignores shell chrome (Progman/WorkerW/DefView/taskbar/Shell_TrayWnd), logging the offending window class+rect. Proven live: probe showed pos frozen + pause in the log before, posRate=1.00x with playing=True for 20 s after (`Engine::CheckFullScreenPause`)
- [x] **A11 · MAJOR** — **HDR10 content renders far too dark on an SDR target** — **fixed 2026-10-07**: `D3D12Renderer.cpp:156` now sets `norm = max(adj.w, 1.0)` (paper white, 203 nits) instead of `masterMax` (1000); the pre-ACES clamp at `:226-238` now only clamps SDR input (`hdrInput ? max(rgb,0) : saturate(rgb)`), so highlights ≥203 nits keep gradation (397→245, 594→250, 1004→253). Before: 203 nits → 150/255 (59% of signal) and every value ≥203 nits collapsed to one code. Verified: exact shader port + **on-GPU end-to-end**: rendered HDR band video through the rebuilt runtime and sampled the screen — measured (145, 204, 231, 245, 249, 252, 253) vs predicted (148, 204, 232, 245, 250, 252, 253) across 40–1004 nits; zero shader errors, presents OK; real LG OLED 4K HDR file decodes on `HW_P010`, back buffer confirmed SDR
- [x] **A12 · MAJOR** — **SDR video is double gamma-encoded (washed out / too bright)** — **fixed 2026-10-07**: `:161-162` (static RGBA) and `:223` (NV12 YUV→RGB) now apply `SrgbToLinear` before the pipeline's `LinearToSrgb`, restoring the shape `4ba29fa` had. Before: source 64→137, **128→188**, 192→225. After: **exact identity** (128→128, all codes) in the shader port
- [x] **A13 · MAJOR** — **A3's fix has since been reverted** — **fixed 2026-10-07** as part of the same diff: `:245` already read `lin = rgb * adj.w` (a parallel edit restored it); verification plus comment correction at `:260-266` documents that `norm` is now paper white so `rgb * min(norm, peak)` restores absolute nits for HDR input. The broken-ffmpeg-reference comment was also removed/replaced with the real justification (ITU-R BT.2408 203 nits ≈ 90% signal; the old reference omitted the output OETF, YAVG 53.3 vs corrected 100.5)
- [ ] **Refuted — "static image path is an exact passthrough"** (do not re-derive): the identity measurement (`128→128`, `(255,0,0)→(255,0,0)`) never touched the shader. Static images go through `SetImageWallpaper` (`wallpaper_cli_main.cpp:423-451`) → registry + `SystemParametersInfoW(SPI_SETDESKWALLPAPER)`, i.e. **Windows draws them**. So it validates neither branch of the pixel shader, and the `extra.z` static branch would in fact double-encode (same A12 shape). Screen-scraping is also unreliable here (window occlusion + DPI); use the shader port instead
- [ ] **Verification tooling (not in repo)** — shader math port + band generators live in `%TEMP%\opencode\rex-verify\` (`shader_port.py`, `make_bands.py`, `client.cjs`, `run_test.ps1`, `sample.ps1`, `snap.ps1`) and will be wiped with temp. Copy into `rex-core/tools/` (or `tools/color/`) before the next machine/temp cleanup. Builds from this session are **not** committed — `617D82E2EBD38352` exe still carries `norm = masterMax`
- [ ] **Build system broken (blocks clean builds)** — no working vcpkg on this machine: `C:\vcpkg\scripts\buildsystems\vcpkg.cmake` is a `FATAL_ERROR` stub, `C:\Users\Moinu\vcpkg` is a broken clone without `vcpkg.exe`. Consequences: any CMake reconfigure dies, and the `vcpkg applocal` post-build step fails (exit 9009), so `ffmpeg/fmt/spdlog` DLLs are not refreshed. **Workaround in use since 2026-10-07:** an offline passthrough toolchain at `%TEMP%\opencode\vcpkg-offline-toolchain.cmake` (outside the repo) pointed at the prebuilt `rex-core/.build/vcpkg_installed`, so `cmake -S rex-core -B rex-core\.build -DCMAKE_TOOLCHAIN_FILE=<that file>` reconfigures normally and source edits no longer need the `generate.stamp` touch. Install real vcpkg to fix properly

### Runtime deep-dive (2026-10-07 audit, unfixed — see `docs/bugs/AUDIT-runtime-deep-dive.md`)

**Baseline implementation order (adopted 2026-10-07 — correctness first, no perf chasing until P0-P3 done):**

- [ ] **P0-1 WorkerW/desktop-layer crash protection** — BUG-001 P0-A/B/C must survive layer reconstruction; add R13 stale-client detach (check every `DuplicateHandle`, revalidate, explicit detach)
- [ ] **P0-2 HDR state/color pipeline [R16 + G3]** — determine actual OS HDR state at init, create SDR+HDR PSOs, correct swapchain; on HDR/topology change refresh state, select PSO/output, resize only when required. Never decide from monitor capability alone
- [ ] **P0-3 SW-decoder pool corruption + per-frame rebuild [R3 + R4]** — HW = P010/NV12 GPU pool, SW SDR = BGRA pool, SW HDR = RGBA16 pool; pool creation one-time per resolution/format generation regardless of HW/SW path
- [ ] **P0-4 D3D12 allocator fencing [R17]** — FrameContext[N] ring, reset a context only after its fence completes
- [ ] **P1-5 Seek/stale-frame clearing [R10]** — add `FrameScheduler::Clear()`, call on explicit seek, loop wrap, recovery seek
- [ ] **P1-6 Stop resets full A/V timeline [R8]** — Stop = decoder seek 0 + audio seek 0 + scheduler clear + position 0
- [ ] **P1-7 Restore loop+speed+volume+mute in one path [R9]** — LOAD and recovery restore all playback properties, not just loop/speed
- [ ] **P1-8 Seek/Close lifetime race [R15]** — single ownership mechanism for decoder lifetime
- [ ] **P1-9 FFmpeg interrupt callback [R14]** — `AVIOInterruptCB` tied to the decoder stop flag so `Close()` cannot hang in `av_read_frame`
- [ ] **P1-10 0-frame/EOF handling [R5]** — bounded EOF attempts plus backoff; 0-frame loop must not hang `Open`, failed seek + loop must not spin
- [ ] **P1-11 Audio ring overflow [R6]** — clamp/downmix to the actual ring channel count
- [ ] **P1-12 Swapchain latency flag [R7]** — `Resize` must preserve `FRAME_LATENCY_WAITABLE_OBJECT`; **P1-13 A/V drift [R20]** — `SetSpeed` reaches audio, position follows decoder PTS
- [ ] **P2-14 Static render-once gating [R2]** — static wallpaper renders once, `frameDirty = false`, no rendering until something changes
- [ ] **P2-15 Cadence control [R18]** — cadence = min(maxFps, decoder cadence); clamp IPC `maxFps` to sane bounds
- [ ] **P2-16 Hot-reload/spectrum outside dirty gate [R19]** — reachable even when the render gate is inactive
- [ ] **P2-17 Protocol hardening [R11 + R12 + R13]** — non-throwing `filesystem::exists(error_code)` + hostile-path validation; zero-length frame = malformed, not EOF
- [ ] **P3 service behavior [G1-G5]** — power awareness (battery saver/display-off/lid); Win+L suspend; topology incl. secondary/DPI/HDR-toggle; audio endpoint re-enumeration + single-audio-owner policy; native named-mutex + protocol versioning (+ G6 bounds, G7 user error surface, G8 counters/bounded logs)
- [ ] **P4 benchmarks** — performance tuning + cross-hardware benchmarks only after P0-P3; GPU/mem numbers are meaningless until the correctness bugs are fixed

- [x] **R1 · CRITICAL** — hot-reload `LoadTexture()` deadlocks the render thread inside `Begin/End` — **fixed 2026-10-07**: hot-reload block moved before `BeginFrame()` + the dirty gate in `Engine.cpp` (also fixes R19 starvation for static scenes, arms dirty on success); new `FrameLockPolicy` (`graphics/renderer/FrameLockPolicy.hpp`) tracks the frame window + owner thread — `BeginFrame` arms, `EndFrame` disarms, `LoadTexture` fails loudly (`InvalidState`) on same-thread reentry instead of self-deadlocking while cross-thread callers still block normally. Verified: `FrameLockGuardTest` 3/3 (RED: C1083 on missing header), full wallpaper suite green except 2 pre-existing fixture-gap failures (`VideoScenePlayback` needs `test.mp4`, `RapidSceneSwitching` expects success for `Z:\missing` — same known test-gap family), live CLI static load + file overwrite → `Hot-reloaded static image` in log, process alive (old code would hang the render thread)
- [x] **R2 · HIGH** — static scenes present at full rate forever — **fixed 2026-10-07**: the dirty flag is now cleared after any presented frame (`Engine.cpp` after `EndFrame`), not just in the video branch; load/hot-reload/transform paths still arm it. Verified: `StaticRenderOnceTest.StaticScenePresentsOnceThenIdles` (RED: 91→180 presents in 1.5 s idle; GREEN: frozen), suite otherwise unchanged (same 2 pre-existing fixture failures), live CLI static: 0.33 s CPU over 20 s idle, file touch → `Hot-reloaded` + one re-present, no hang
- [ ] **R3 · CRITICAL** — software decode uploads RGB bytes into a YUV pool, corrupt frames; the promised RGBA16 pool was never implemented (`FfmpegVideoDecoder.cpp:707-709,935-949`)
- [ ] **R4 · CRITICAL** — `EnsurePool` fast-path requires `m_processor`, null on software decode, so the pool is rebuilt every frame and the renderer 8-slot cache exhausts, freezing video (`FfmpegVideoDecoder.cpp:683-687,742-760,913`)
- [ ] **R5 · HIGH** — 0-frame file with loop hangs `Open` forever; failed seek + loop busy-spins at 100% CPU (`FfmpegVideoDecoder.cpp:593-602`, `VideoPlaybackController.cpp:280-282`)
- [ ] **R6 · HIGH** — audio ring overflows on multichannel sources: stride uses `max(frameCh, m_channels)` vs a ring allocated for `m_channels` (`AudioPlayer.cpp:331-352`)
- [ ] **R7 · HIGH** — `SwapChain::Resize` strips `FRAME_LATENCY_WAITABLE_OBJECT`, ~1 fps stutter after any display-change resize (`SwapChain.cpp:126-130` vs `:46`)
- [ ] **R8 · HIGH** — `Stop()` splits A/V + position: audio rewinds to 0 while video and the clock resume mid-stream (`VideoPlaybackController.cpp:145-152`, `AudioPlayer.cpp:143-147`)
- [ ] **R9 · HIGH** — `LOAD`/recovery drops volume and mute, only loop+speed are re-pushed (`Engine.cpp:427-428`); per-monitor videos have no volume/mute at all
- [ ] **R10 · HIGH** — `Seek` never clears `FrameScheduler`: a stale pre-seek frame is presented (`VideoPlaybackController.cpp:180-194`, `FrameScheduler.cpp:11-18`)
- [ ] **R11 · MAJOR** — unhandled `std::filesystem::exists` throw on hostile paths crashes the runtime (`wallpaper_cli_main.cpp:815`); harden `Utf8ToWiden` with `MB_ERR_INVALID_CHARS`
- [ ] **R12 · MAJOR** — zero-length frame conflated with EOF: one bad frame silently kills the session (`wallpaper_cli_main.cpp:508,707`). Answer `malformed frame`, continue
- [ ] **R13 · MAJOR** — `DuplicateHandle` results ignored; stale client process never revalidated, no detach (`wallpaper_cli_main.cpp:1056-1076`)
- [ ] **R14 · MAJOR** — `Close()` can hang in blocking `av_read_frame`, no interrupt callback (`FfmpegVideoDecoder.cpp:609`)
- [ ] **R15 · MAJOR** — `Seek` vs `Close` race on `m_decoder`; unlocked raw reads in diagnostics
- [ ] **R16 · MAJOR** — HDR output state snapshot-once: stale PSO, late `SetForceSdr`, no mid-session toggle detection (`D3D12Renderer.cpp:374,506,1169`)
- [ ] **R17 · MAJOR** — single command allocator reset per frame with no fence gating (`D3D12Renderer.cpp:1102-1108`)
- [ ] **R18 · MEDIUM** — `PaceLoop` ignores decoder cadence; idle loop wakes at `maxFps`; IPC `maxFps` unclamped (`Engine.cpp:1504`, `wallpaper_cli_main.cpp:748`)
- [ ] **R19 · MEDIUM** — hot-reload + spectrum blocks starve behind the P16 gate (`Engine.cpp:1449-1474` vs `:1415-1418`)
- [ ] **R20 · MEDIUM** — `SetSpeed` never reaches audio; engine position is wall-clock, never decoder PTS (drifts on speed/loop/recovery)
- [ ] **G1 · MISSING** — no power awareness (battery/display-off/lid): full-rate decode/upload/present forever
- [ ] **G2 · MISSING** — no session-lock pause (Win+L keeps presenting behind the lock screen at full rate)
- [ ] **G3 · PARTIAL** — display-topology holes: HDR toggle, secondary-only changes and DPI (hardcoded 1.0) never reconcile
- [ ] **G4 · PARTIAL** — audio endpoint loss is permanent until reload; layered per-monitor videos mix with no policy
- [ ] **G5 · MISSING** — no protocol versioning; no second-runtime guard (two runtimes fight over the WorkerW layer)
- [ ] **G6 · PARTIAL** — no resource bounds (8K/huge files surface errors late); hung compositor stalls every monitor
- [ ] **G7 · PARTIAL** — failures only `console.warn`d in the UI: no user-visible error surface for failed wallpapers
- [ ] **G8 · PARTIAL** — observability gaps: no per-stage/per-compositor/audio/present counters; native logs grow forever
### Module platform (`rex-host`, not on the wallpaper path)

- [x] **B1 · CRITICAL** — `rex.settings` dereferenced a null service → startup access violation — **fixed 2026-10-07**: `ModuleOrchestrator::initialize` now publishes `EventBus` + `ConfigurationStore` into the `ServiceContainer` (as non-owning views; ownership stays with the orchestrator), and `SettingsModule::initialize` fails loudly instead of dereferencing null. Verified: `rex-core` builds, module/core suites green
- [x] **B3 · CRITICAL** — `std::stoi` on the profile auto-switch thread → `std::terminate`; plus a double `unlock()` UB — **fixed 2026-10-07**: numeric parse is validated and wrapped (a bad value is logged and skipped), matches are collected under the lock and applied outside it, and every apply is exception-guarded so a failing profile can never abort the host (`ProfileManager.cpp`)
- [x] **B2 · CRITICAL** — shutdown wiped all settings — **fixed 2026-10-07**: shutdown() now merges with the on-disk config, carrying settingsJson and restartCount through (`ModuleOrchestrator.cpp`). Verified: settings suites green
- [x] **B4 · CRITICAL** — DLL unloaded under live objects — **fixed 2026-10-07**: reloadModule() now calls mod->shutdown() (exception-guarded), destroys the old instance before FreeLibrary, serializes reload against all snapshot paths via reloadMutex_, and re-inserts the id into startupOrder_ (`ModuleOrchestrator.cpp/hpp`). Verified: builds, module/core suites green
- [x] **B5 · CRITICAL** — detached worker capturing `this` — **fixed 2026-10-07**: workers are tracked and joined in the destructor (with cancellation first); a cancelled command no longer reports success; finished workers are reaped (`CommandExecutor.cpp/hpp`). Verified: command suite 82/82
- [x] **B6–B8 · MAJOR** — shortcut self-deadlock, unsynchronized ServiceContainer, non-atomic config writes — **fixed 2026-10-07**: events/callbacks fire after the lock is released (`ShortcutManager`); `ServiceContainer` is mutex-guarded; `ConfigurationStore::saveModule` holds the lock across read-modify-write and writes via temp-file + atomic `MoveFileExA` replace (`ConfigurationStore.cpp`). Caught my own re-lock bug in loadModule via the round-trip test. Verified: settings 40/40, command 82/82

### Earlier findings

- [ ] **Wallpaper z-order jump + input hang (BUG-001)** — after apply, wallpaper jumps above icons on desktop mouse interaction, blocks input ~1s, restores. **Diagnosed 2026-10-06** — root cause + fix plan: [`docs/bugs/BUG-001-workerw-zorder-input-hang.md`](docs/bugs/BUG-001-workerw-zorder-input-hang.md)
  - [x] **P0-A non-destructive repair** — **done 2026-10-06**: alive-but-mispositioned layers are re-anchored below the icon layer instead of destroyed+rebuilt (`WorkerWManager::TryReanchorLayer`), recovery policy extracted to `platform/windows/LayerRepairPolicy.hpp` (7 unit tests), `WM_SPAWNWORKERW` storm now only fires when a window is actually gone (`NeedsWorkerWSpawn`). No more full engine reload for a z-order fix. Verified: `rex-core-tests` 7/7 policy + 8/8 existing desktop-host tests pass, runtime builds
  - [x] **P0-B input-safe synthetic layer** — **done 2026-10-06**: `REXSyntheticWorkerW` created with `WS_EX_NOACTIVATE | WS_EX_TRANSPARENT` + a hard verify invariant so the styles cannot silently regress. Desktop clicks now pass through even if the shell leaves our layer above the icons
  - [x] **P0-C wire `DesktopWindowThread`** — **done 2026-10-07**: `DesktopWindowThread` is now instantiated per host (first `Acquire`, stopped in `Release`) and **every mutating HWND op is marshalled through it** — discovery, create+reparent, destroy, synthetic-layer destroy, re-anchor. `src/platform/windows/DesktopWindowThread.cpp` was missing from `CMakeLists.txt` entirely (dead code, 4 unresolved externals) and is now listed. Read-only queries stay on the calling thread; `Send()` timeouts are treated as failures, and an inline fallback keeps the host alive if the thread cannot start. Verified: runtime drives a full INIT→monitor→SHUTDOWN cycle with `DesktopWindowThread` start/exit in the log; `rex-core-tests` 34/34, `rex-module-tests` 11/11
  - [ ] **P1-D instrumentation** — still open: WinEvent reorder hook + WndProc message logging to pin down which Explorer action moves the layer (hypothesis H-1/H-2 in the report)
- [x] **ProgmanHost hardened** — **fixed 2026-10-07**: own DesktopWindowThread with all create/destroy/reposition marshalled (`ProgmanHost.cpp/hpp`); GDI brush leak per Acquire fixed (stock brush); PostQuitMessage removed from the child WndProc. MultiMonitorDesktopHost still open (only if it becomes the active path — it wraps WorkerWManager, whose windows are already covered). Verified: runtime builds, desktop-host suite green
  - [ ] **`MonitorCompositor` windows** — still open: created on the render/main thread and destroyed on whichever thread tears the engine down (audit finding A-set #6); needs the same window-thread ownership
  - [ ] Manual soak verification: apply a wallpaper, click/hover the desktop repeatedly, confirm no z-order flap, no input freeze, telemetry `presents` never resets
- [ ] **HDR present bug** — present-to-screen failure under HDR; 14 diagnostic logs (`hdr_*.log`), harness (`hdrprobe.obj`, `present_repro.obj`), notes in `fix.md`. Check against A3 first — may be the same root cause
- [ ] **Widget WebView2 JS bridge dead** — `HandleWebMessage` stubbed; MSWRL::Callback init failure in `rex-core/src/modules/Widgets/services/WidgetHost.cpp`
- [ ] **Uncommitted engine WIP** — `D3D12Renderer.cpp`, `WorkerWManager.cpp/.hpp` modified since recovery point `1ca6195`; finish or revert, then commit (BUG-001 + A3/A4 fixes build on this diff)
- [ ] **FPS counter hardcoded** — inspector always shows "60" (see `#project_status.md` → Performance)
- [ ] **`runtime.log` 0 bytes** — spdlog file sink never flushes (`%TEMP%\REX\runtime.log`), blocks all engine post-mortem debugging; seen while diagnosing BUG-001 (report §P1-E)

## 🔥 Now — active WIP (finish these first)

- [ ] **Color accuracy (HDR wrong / SDR washed out) — fixed + verified 2026-10-07**: A11 (paper-white norm, pre-ACES clamp lifted), A12 (SrgbToLinear restored), A13 (comment corrected). Built clean (MSBuild, no warnings), shader port exact, end-to-end GPU band test green. Remaining: repack `app/dist-exe` + reinstall, then one eyeball A/B vs Media Player on the same HDR frame
- [ ] Stabilize ffmpeg video pipeline (GPU-native plan from `461b79c`) — decode → upload → present path
- [ ] Re-audit `#project_status.md` against post-July code (shell UI moved to `dump/`, Electron app is now the UI)
- [x] Reconcile docs drift — **done 2026-10-06**: README status fixed, `everything.md` + `TODO.md` + `AGENTS.md` created

---

## Roadmap (README phases)

- [ ] **Phase 0–1 exit criteria re-validation** — WorkerW host survives explorer restart; widget MVP usable daily
- [ ] **Phase 2** — SDK alpha: API, CLI, sandbox, docs; stranger builds a widget from docs alone
- [ ] **Phase 3** — Wallpaper + WindowFX parity with Rainmeter + Wallpaper Engine
- [ ] **Phase 4** — Automation engine: triggers → actions end-to-end
- [ ] **Phase 5** — Public marketplace: publish/install/review, 3rd-party plugin installed safely
- [ ] **Phase 6** — Platform maturity: diagnostics, themes, a11y, i18n

---

## Wallpaper engine (from `phases.md`)

- [ ] **Phase 3 — Library:** import, metadata, thumbnails, favorites, collections
- [ ] **Phase 5 — Performance:** pipeline/texture cache, upload+descriptor heap reuse, GPU profiling
- [ ] **Phase 6 — SDK:** `IWallpaperEngine` / `IWallpaperScene` / `IWallpaperLoader` (no Win32 handles exposed)
- [ ] **Phase 8 — Effects:** blur, acrylic, mica, particles, shader effects
- [ ] **Phase 9 — Plugin system:** stable SDK, sandboxed plugins, version compatibility
- [ ] **Phase 10 — Production:** leak detection, explorer recovery, crash recovery, CI/CD, installer, docs
- [ ] GIF/APNG renderer · SVG wallpapers
- [ ] Playback: pause on fullscreen/battery/idle/RDP, FPS limit, background throttling, auto-resume, span/mirror wallpaper
- [ ] Preview: live/fullscreen preview, thumbnail generation, metadata
- [ ] Library management: collections, favorites, tags, search, sort, filter, smart collections, recent, trash, duplicate detection
- [ ] Slideshow

## Widgets

- [ ] Widget SDK / API / sandbox
- [ ] Widget store + installer
- [ ] Desktop interactions: drag, snap, align, lock, group, layers, per-monitor
- [ ] Built-in widgets: Calendar, Notes, Music, RSS, Battery, Agenda, World Clock, Process Monitor, GPU Temp

## Personalization (StartAllBack/Seelen-class)

- [ ] Taskbar: transparency backend, blur, grouping, animation
- [ ] Start menu: replace backend, size, transparency, search, power menu
- [ ] Explorer: ribbon, navigation pane, compact-mode backend
- [ ] System tray: layout, overflow, icon spacing
- [ ] Window: rounded corners, shadow, animation, snap, transparency
- [ ] Win32PlatformAdapter — real hooking (🟡)
- [ ] Profiles: predefined (gaming/battery/work/movie/night) + auto-switching

## Windhawk-like mod system

- [ ] Mod install/enable/disable/configure/update
- [ ] Explorer / Taskbar / Start Menu / DWM / Shell hooks
- [ ] Mod repository: browse, search, ratings, versioning

## Marketplace

- [ ] Assets: wallpapers, widgets, themes, icons, fonts, cursors, plugins, shaders, scenes
- [ ] Ratings/reviews, screenshots, changelog, author pages, version history
- [ ] Download queue (progress/resume/cancel), auto-update
- [ ] Finish `CurlMarketplaceClient` (🟡) → auth, catalog, install flow

## Theme system

- [ ] Theme editor, preview, export/import, inheritance, sharing, variables, presets

## Asset manager

- [ ] Fonts, icons, cursor packs, sounds, wallpapers, theme packages

## Backup & cloud

- [ ] Full backup / full restore
- [ ] Cloud sync (spec exists: `docs/REX-Cloud-Sync-Spec.md`)

## Automation engine

- [ ] Schedule wallpaper/themes, trigger profiles, battery rules, time rules, game detection
- (Full spec: `docs/res/RES-Automation-Engine.md`)

## Dashboard

- [ ] Recent activity, quick actions, favorites, resource usage, running modules, news, updates, shortcuts

## Performance monitoring

- [ ] CPU/GPU/RAM/VRAM/temperature/real FPS counter/frame time/module usage/process monitor
- [ ] Replace hardcoded "60" FPS in inspector

## Plugin SDK & package manager

- [ ] SDK packaging + publishing (APIs/lifecycle/events ✅)
- [ ] Package install/remove/versioning/rollback (dependency resolution ✅)
- [ ] Permission manager, download manager, thumbnail service, package installer

## Cross-cutting systems

- [ ] Analytics/telemetry, crash reporting, recovery mode, safe mode
- [ ] Undo/redo, clipboard, history managers
- [ ] Background task scheduler
- [ ] UpdateManager (🟡 stub → real)

## Developer tools

- [ ] Module inspector, event viewer, FPS overlay, GPU/memory profiler
- [ ] Theme/search/renderer inspectors
- [ ] Logger hardening (spdlog linked, basic only)

## UI track

- [ ] Decide/confirm UI direction: Electron app (current) vs C# shell vs native — then archive losers cleanly
- [ ] Port remaining Surface System materials/tokens into the Electron app
- [ ] Settings / Developer / Marketplace pages (currently placeholders)
- [ ] Widget drag/snap/lock UX in the Electron workspace
- [ ] E2E visual regression from `e2e_a/b.png` baselines

## Priority 3 (future)

- [ ] Wallpaper Editor (timeline/layers/audio/effects/export)
- [ ] Theme Editor (visual, live preview, publish)
- [ ] Widget Designer (visual editor, packaging)
- [ ] AI: wallpaper generation, theme creation, automation, recommendations

---

### Legend

`[ ]` not started · see `🟡` items in `#project_status.md` for partial work · move completed items to `everything.md` ✅
- [x] **A15 · MAJOR** — fullscreen flash (~1 s XamlWindow overlay) froze the wallpaper and corrupted the position readout (20 s → 6 s) — **fixed 2026-10-07**: the pause now fires only after the fullscreen state persists 1.5 s, and pause/resume no longer rewind the clock — elapsed time is folded into a position base (`m_positionBase`) since Stopwatch restarts from zero, so resume continues instead of rewinding while the decoder keeps its own PTS. Proven live: PAUSE 3 s froze pos at 6.06, PLAY resumed to 10.08 (not 0)
