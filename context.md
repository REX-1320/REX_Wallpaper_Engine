# REX — Working Context (agent handoff)

> Created 2026-10-19 from the session compaction dump + live state.
> Last updated: 2026-10-19 15:40 (+05:30) — model: DeepSeek V4.1 Flash Free (`opencode/deepseek-v4.1-flash-free`).
> **Living document: update the "Current state" and "Remaining" sections after every finished work unit.**
> Companion docs: `TODO.md` (remaining + bug backlog), `everything.md` (done inventory), `docs/bugs/AUDIT-2026-10-17-quality-audit.md` (source findings).

---

## Mission

Make the wallpaper engine perfect — 4K video playback stutter-free with faithful colours. Bug fixes over features.
Scope banner: engine (`rex-core/` wallpaper stack) + gallery/apply path (`app/`) only. Widgets/taskbar/themes/marketplace deferred.
User-sequenced end state: finish the remaining Batch 3–4 audit fixes, **then** the two user-reported IMAX-clip bugs (Odyssey green tint — **fixed 2026-10-19, unit 18**; Oppenheimer colours — **next**).

## Standing rules (from AGENTS.md)

1. **Never edit `rex-core/CMakeLists.txt`.** New tests go only into already-registered test files.
2. After EVERY finished work unit, update **`TODO.md`** AND **`everything.md`** (done entry + counts + date). `#project_status.md` only when a feature ships.
3. Pipeline per unit: fix → build (`rex-wallpaper-tests`) → tests (ctest, never the raw exe) → RED/GREEN evidence where feasible → runtime relink (only when the app is closed: 0 matching processes) → trackers → **2 commits** (code+tests, trackers).
4. Bugs over features. Leave `app/src` WIP deletions + untracked research files (`--help`, `cli-test.*`, `.opencode/`) untouched unless a batch explicitly targets them.
5. Don't edit D3D12Renderer colour/shader code without confirming scope first.
6. `far` is an empty windows.h macro — never use as identifier. Always capture `BUILD_EXIT` separately; a failed build lets ctest run a stale exe.

## Environment facts

- Build: `cmake --build .build --config Release --target rex-wallpaper-tests` (workdir `rex-core`); runtime: `--target rex-wallpaper-runtime`.
- Test: `ctest --test-dir .build -C Release --output-on-failure`. Suite: 803 tests (804 entries, one disabled), pass line format "100% tests passed out of N".
- Test exe: `rex-core/.build/Release/rex-wallpaper-tests.exe`. Runtime exe: `rex-core/.build/Release/rex-wallpaper-runtime.exe`.
- Tests never call `Logger::Initialize` (no engine log from ctest). Rust note: logs to `%TEMP%\REX`.
- Machine: RX 6700 XT, 3840×2160@120 Hz, MPO disabled. In-repo media fixtures: `bench-media/` (e.g. `mpeg4_320x240.mp4`).
- Only the final ctest log per unit is kept (others are gitignored).

## Campaign state (Batch 3 audit fixes)

### Done (commits on `main`, nothing pushed)

- Units 1–7: `ae806fd` … `ba536f5`/`db264a5` (Batch 3 P1/P2s: recovery gates, locks, shared-fence sequence, etc.).
- Unit 8: `5b921ad` + `77cd0eb` — FrameScheduler Clear-vs-Tick lost update (§5.4 #4; PV9 dead `ResetPresentationClock`, PV14 SPSC contract). 773/773.
- Unit 9: `5cbd4a6` + `868d9e9` + `42b6158` — playback EOF restart latch (§5.4 #5), `VideoManager::Shutdown` self-contained (§5.4 #7), per-monitor tick target (§5.4 #8); SpeedClock flake diagnosed (ambient auto-pause) + hardened. 777/777.
- Unit 10: `534b906` + `dfeea93` — decoder pool atomic build (§5.2 #2), renderer slot-cache eviction (§5.2 #3/F1), SW slot lease (§5.2 #4/§5.4 #3), partial-QI leak (§5.2 #6), `PickSlotOldestSafe` public (#15 tie-in). New test `SoftwareDecodePoolTest.SoftwarePathLeasesPoolSlots` RED→GREEN. 778/778. Runtime relinked 2026-10-19 11:10:30.
- Unit 11: telemetry sleep slicing (§5.5 #1), `TryGetTelemetry` try-lock for watchdog + sampler (§5.5 #2), oversized-prefix teardown vs zero-length continue (§5.5 #3). New tests `FrameLengthTest.*` (Empty/Oversized) + `EngineTelemetryTest.TryGetTelemetrySucceedsWhenUncontended`; live protocol smoke (zero→continue→SHUTDOWN; oversized→close in 41 ms). 779/779. Runtime relinked 2026-10-19 11:38:37.
- Unit 12: `AudioCapture` COM ref tracked (`m_comInitialized`, §5.3 #4/#5) + MTA apartment on capture thread (#6) + `OpenClient()`/`ReopenDevice()` device-change recovery (#7) + SILENT/null → zeros (#8); `AudioPlayer` getters atomics (#9), `DoSeek` failure keeps state (#10). New tests `AudioCaptureRecoveryTest.*` + `AudioCaptureSilenceTest.*` (compile-RED → green). 782/782. Runtime relinked 2026-10-19 12:05:32.
- Unit 13 (app): quit fallback 3→5 s + cleared on exit (§5.10 #4), INIT failure drops the wedged process (#6), heartbeat capability-probed + armed with miss limit 3 (#7), live updates merged into `lastApplied` + debounced persist via pure `startup.mergeLiveUpdate` (#5), login item first-run-only (#8). Evidence: node checks 11/11, live `verify-supervisor.cjs` incl. new arming assertion, lint/tsc clean. No native change; suite stays 782/782.
- Unit 14 (graphics): compositor sync/state hardening — main-queue frame fence + bounded GPU waits (2 s) + device-removed latch (§5.8 #3/#5), layer-texture tracked state (#4) + per-back-buffer tracker (#9), SRV slot recycle on replace (#6), shared-handle import validated + SRV format/dims from `GetDesc()` (#7), documented producer/consumer contract (#8). 9 new CPU-only tests (compile-RED → green). 791/791. Runtime relinked 2026-10-19 12:30:26.
- Unit 15: `296dbce` + this trackers commit — native P3 residual sweep (audit §5.1 #10–#13/#15, §5.3 #11/#13–#15, §5.4 #11/#13/#15, §5.5 #4–#12, §5.6 #9–#13, §5.8 #11): engine DXGI-factory reuse + `WideCharToMultiByte` guard + dead-state/`m_pressureScale` removal + unified `GpuMemoryQuery` + PaceLoop comment; audio resample scratch (no MMCSS allocation) + `PropVariantClear` on all successful types + `ReleaseBuffer` HRESULT + CoInit pairing + dead members; playback `StopMonitor`→`Stop` + `RefreshDecodeAheadPolicy` after adaptive switch + `FS15` rename; runtime `SET_*` require `value`, Json non-finite/out-of-range guards (1e999 UB), GET_FRAME_SURFACE clamp+`duplicated`, brightness/contrast/saturation wired through `Engine::SetProperty`, `RuntimeSettings` 4 MiB cap + bool setters, settings → `%LOCALAPPDATA%\REX\settings.json`, `WidenToUtf8` guard, log-flood cap, command-list header; renderer cb repack (root signature is 64/64 — 16 DWORDs fails `CreateRootSignature`; dead lanes dropped, SDR white now uploaded) + `InitializeCore` + `TicksToMicros` + HRESULT checks; compositor `REX_BG_LOG`. Evidence: `JsonNumericTest.*` 5 new (RED pre-fix 3/5 fail → GREEN 5/5), `DisplayCadence.TicksToMicrosUsesTheRealFrequency` compile-RED C2039/C3861 → GREEN; **797/797** (`ctest_p3_1019.log`); runtime relinked 2026-10-19 14:07 (771,584 B).

- Unit 16 (app): `beafc6e` + this trackers commit — app P3 sweep (audit §5.10 #9–#16): renderer watchdog wired (preload `renderer:ping` 2 s heartbeat → `supervisor.recordRendererPing`; `render-process-gone` → `handleRendererGone`; show/restore re-baseline; minimized skip in the stale tick); crash-report `appLog` path aligned to `<temp>\rex-app.log` (#10); `rex-minidump.exe` bundled via `extraResources` (#11); spawn failure emits `exit(null, {pid, spawnError})` → `runtime-spawn-failed` report + bounded recovery (#12); exit-event pid captured before `_teardown` (#13); new pure `path-guard.cjs` `isPathInside` for delete-thumbnail (#14); dead preload APIs + orphaned main IPC handlers deleted, `getCapabilities` wired through new pure `capabilities.js` into `desktop.js` (#15); UI no-ops removed — Actions' 8 dead buttons (Open Folder wired), Playback transport group, Audio duplicate Mute/Volume (#16). Evidence: node harness 46/46 GREEN with RED cycle on the pre-fix tree (20 FAIL + path-guard RED 3 FAIL), `node --check`, lint/tsc/vite build green. No native change; suite stays 797/797; no relink. Live app re-watch pending restart. New P3 backlog: rex-minidump has no CMake target (packaging ships a stale-but-valid exe).

- Unit 17 (§6 hygiene): orphaned `wallpaper/renderer/` (15 files, in no CMake target, cannot compile) `git mv` → `dump/wallpaper-renderer/` + README (origin, §5.6 #7 latent bugs, revival notes — audit §5.6 #6/#7/§6); root clutter staged out of the repo to `%TEMP%\opencode\root_clutter_2026-10-19\` (6 `.obj`, 4 `repack*.log`, e2e/wall pngs ~10 MB — the tracked `wall_*` pair now `git rm`'d, still in history — `--help` ×2, `rex-runtime.log`, `rex-cli.log`, `test_settings_config.json`, `rex-core/cli-test.err/.out`; kept: `hdr_*.log`, PresentMon CSV, `_research_*`/`decode_raw*`/`fps.md`, `bench-media/`, `sessions/`); stale docs fixed (`#project_status.md` Shell UI banner'd RETIRED + stub-adapter rows ✅→🟡; `everything.md` repo-map/harness/screenshot rows; A1/A2 overstated-closure corrections in TODO + everything). New findings: native `playlist/` orphan (uncompiled + unreferenced — owner call, NOT archived), CMake-blocked remnant list consolidated (dead compiled modules incl. §5.8 #10/#12, unbuilt tests incl. `test_compositor_render.cpp`, §5.9 #10). No code changed; build no-op green; **797/797** (`ctest_unit17_1019.log`); no relink.

- Unit 18 (IMAX bug 1 — Odyssey green tint): `PoolTextureFormat` gained a third param `surfaceFormat` (pass `desc11.Format` at the HW call site) — the HW pool now mirrors the decoder surface (P010 stays P010 for 10-bit SDR; NV12→NV12; unhandled formats keep the flag fallback); the surface log is now per-change with `pool=`/`copy=match|MISMATCH` (the old once-per-session static is exactly why this bug stayed invisible). Renderer shader: P010 branch gained the SDR case (BT.601/709 + `SrgbToLinear`, mirroring the 8-bit branch) and the `extra.x > 1.5` HDR proxy was removed at all four downstream sites (SDR clamp, HDR-out branch, PQ-out matrix select, ACES gate) — replaced by `mode.y > 0.5` (transfer), never the pool format. Tests: 5 pure mapping + `VideoPipeline.TenBitSdrDecodePublishesP010PoolTexture` (runtime discriminator; in-repo 10-bit SDR fixture; RED proved consumer-visible NV12 where P010 required — 103 vs 104) + rewritten `ColorPipeline.PoolFormatMirrorsDecodeSurfaceNotTransfer` (was a vacuous always-true pin). Fixture `bench-media/hevc10_sdr_320x240.mp4` (Main 10, bt709/tv; untracked; override `REX_TEST_VIDEO_10BIT_SDR`). Shader validated with fxc `ps_5_0`; **803/803** (`ctest_odyssey_1019.log`); runtime relinked 2026-10-19 15:16 (772,608 B). Live (headless runtime against the real `TheOdyssey_IMAX_TLR-2…mkv`): `fmt=P010 (0x68) hdr=false pool=P010 copy=match`, frames flowing, clean shutdown; user live look: video + colours correct. New P3 backlog: shutdown-time first-chance VEH 0x71A in `WorkerWManager`'s `WTSUnRegisterSessionNotification` (146/176 historical crash.log blocks — benign, diagnostic noise).

### Remaining (this session's mandate — in order)

1. **Owner call — CMake-blocked hygiene remnants:** dead *compiled* modules removal (§5.7 #11 stubs + `render_scheduler` + `unique_handle`; `graphics/animation|effects` — the §5.8 #10/#12 defects live there; `profiles/**`, `modules/Sync/*`, `WidgetRuntime.cpp`, `UpdateManager.cpp`, `desktop_personalization/**` factory, `ModuleManager.cpp`, Search semantic path), unbuilt-test wiring (`test_compositor_render.cpp` = the only D3D12 compositor smoke — §5.6 #8; `test_startup.cpp`; legacy `test_workerw.cpp` tied to unbuilt `src/workerw.cpp`; superseded `test_settings.cpp`), test-infra temp writes (§5.9 #10). All need `rex-core/CMakeLists.txt` edits — deferred by the standing hard rule. Plus the **native `playlist/` orphan** (wire or prune — new finding, NOT archived).
2. **Leftover native P3 refactors (deferred)** — §5.5 #13 (`RunRuntime`/`RunBenchmark` split), §5.5 #14 (protocol testability; needs a new TU ⇒ CMakeLists-blocked, owner call), §5.6 #8 (offscreen D3D12 tests — pairs with the `test_compositor_render` wiring).

### After the batches (user-sequenced, do NOT start early)

- **Odyssey green tint — DONE (unit 18, 2026-10-19).** Pool mirrors the decoder surface (`desc11.Format`) + P010 SDR shader path; live-confirmed on the actual clip (`fmt=P010 hdr=false pool=P010 copy=match`).
- **Oppenheimer colours — NEXT (unit 19).** Player A/B in mpv/VLC + `capture_wall.ps1` capture vs `opp_frame_709.bmp` (`C:\Users\Moinu\AppData\Local\Temp\opencode\`).

## Key file pointers

| Area | Path |
|---|---|
| Engine | `rex-core/src/wallpaper/engine/Engine.cpp` (GetTelemetry ~line ?, AutoPause ~1911, occlusion ~2111) |
| Playback | `rex-core/src/wallpaper/playback/VideoPlaybackController.{hpp,cpp}`, `FrameScheduler.hpp/.cpp` |
| Video | `rex-core/src/wallpaper/video/VideoManager.{hpp,cpp}` |
| Decoder | `rex-core/src/wallpaper/decoder/FfmpegVideoDecoder.{hpp,cpp}` |
| Audio | `rex-core/src/wallpaper/audio/AudioPlayer.{hpp,cpp}`, `AudioCapture.{hpp,cpp}` |
| Runtime | `rex-core/src/wallpaper_cli_main.cpp`, `rex-core/src/wallpaper/runtime/*` |
| Renderer | `rex-core/src/graphics/renderer/D3D12Renderer.{hpp,cpp}`, `CommandQueue.cpp`, `SwapChain.hpp` |
| Compositor | `rex-core/src/graphics/compositor/compositor.cpp` |
| App | `app/electron/main.cjs`, `app/electron/desktop.js`, `app/electron/wallpaperRuntime.cjs`, `app/electron/supervisor/*.cjs` |
| Tests | `rex-core/tests/wallpaper/test_*.cpp` |

## Audit section map

`docs/bugs/AUDIT-2026-10-17-quality-audit.md`: §5.1 Engine (141), §5.2 Decoder (165), §5.3 Audio (188), §5.4 Playback & video (217), §5.5 Runtime & protocol (241), §5.6 Renderer (269), §5.7 Core/platform (292), §5.8 Graphics (313), §5.9 Module platform (333), §5.10 App (354), §6 hygiene (392), §7 batches (437).

## Current state

**Working on:** IMAX bug 1 of 2 DONE — **unit 18 (Odyssey 10-bit-SDR green mess) fixed and live-confirmed 2026-10-19**: the decoder pool now mirrors the actual surface format (`desc11.Format`), so Main10/bt709 SDR clips keep their P010 pool (live: `fmt=P010 (0x68) hdr=false pool=P010 copy=match`) and format-mismatched copies are impossible; P010 gained the SDR shader path and the pool-format-as-HDR proxy is gone. Tests: 5 pure + runtime P010 discriminator (RED 103 vs 104) + rewritten colour pin; fxc `ps_5_0` clean; **803/803**; runtime relinked 15:16 (772,608 B); user live look: video + colours correct. **Next: unit 19 — Oppenheimer on-screen colours (mpv/VLC A/B + `capture_wall.ps1` vs `opp_frame_709.bmp`).** Standalone owner calls still filed: CMake-blocked hygiene remnants, `playlist/` orphan, rex-minidump CMake target; new P3 found during the live run: shutdown-time first-chance VEH 0x71A in `WorkerWManager`'s `WTSUnRegisterSessionNotification` (benign, diagnostic noise). Live app re-watches for units 13/16 pend an app restart.
**Handoff updated:** 2026-10-19 15:40 (+05:30) by DeepSeek V4.1 Flash Free.
