# REX — Working Context (agent handoff)

> Created 2026-10-19 from the session compaction dump + live state.
> Last updated: 2026-10-19 12:38 (+05:30) — model: DeepSeek V4.1 Flash Free (`opencode/deepseek-v4.1-flash-free`).
> **Living document: update the "Current state" and "Remaining" sections after every finished work unit.**
> Companion docs: `TODO.md` (remaining + bug backlog), `everything.md` (done inventory), `docs/bugs/AUDIT-2026-10-17-quality-audit.md` (source findings).

---

## Mission

Make the wallpaper engine perfect — 4K video playback stutter-free with faithful colours. Bug fixes over features.
Scope banner: engine (`rex-core/` wallpaper stack) + gallery/apply path (`app/`) only. Widgets/taskbar/themes/marketplace deferred.
User-sequenced end state: finish the remaining Batch 3–4 audit fixes, **then** the two user-reported IMAX-clip bugs (Odyssey green tint, Oppenheimer colours).

## Standing rules (from AGENTS.md)

1. **Never edit `rex-core/CMakeLists.txt`.** New tests go only into already-registered test files.
2. After EVERY finished work unit, update **`TODO.md`** AND **`everything.md`** (done entry + counts + date). `#project_status.md` only when a feature ships.
3. Pipeline per unit: fix → build (`rex-wallpaper-tests`) → tests (ctest, never the raw exe) → RED/GREEN evidence where feasible → runtime relink (only when the app is closed: 0 matching processes) → trackers → **2 commits** (code+tests, trackers).
4. Bugs over features. Leave `app/src` WIP deletions + untracked research files (`--help`, `cli-test.*`, `.opencode/`) untouched unless a batch explicitly targets them.
5. Don't edit D3D12Renderer colour/shader code without confirming scope first.
6. `far` is an empty windows.h macro — never use as identifier. Always capture `BUILD_EXIT` separately; a failed build lets ctest run a stale exe.

## Environment facts

- Build: `cmake --build .build --config Release --target rex-wallpaper-tests` (workdir `rex-core`); runtime: `--target rex-wallpaper-runtime`.
- Test: `ctest --test-dir .build -C Release --output-on-failure`. Suite: 782 tests (783 entries, one disabled), pass line format "100% tests passed out of N".
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

### Remaining (this session's mandate — in order)

1. **P3 residuals (§5.8 #10–#12 + §5.1 #10/#11/#12/#13/#15 + §5.6 + §5.4 remaining P3s + §5.3 #11/#13/#14/#15 + §5.10 #9–#16)** — compositor: hardcoded `rex_bg_log.txt` path, effect-renderer capability contract, animation-engine nits; engine: WideCharToMultiByte guard, DXGI factory per monitor, dead state, docs, dup helper; renderer residuals; audio render-callback scratch/ReleaseBuffer/propvariant/CoInit P3s; app dead code/log-path/pid/minidump/delete-thumbnail/preload/UI no-ops.
2. **Batch-4 hygiene (§6)** — orphaned `wallpaper/renderer/` archive (git mv, not delete), dead modules, unbuilt tests, root clutter; tooling only where it does not touch `CMakeLists.txt` (hard rule).

### After the batches (user-sequenced, do NOT start early)

- **Odyssey green tint**: pool format must come from the actual `desc11.Format` (P010) + a P010→SDR renderer path (or VP convert). Location: `FfmpegVideoDecoder.cpp` `PoolTextureFormat` + hw copy path; renderer `m_inputFormat` handling.
- **Oppenheimer colours**: player A/B in mpv/VLC + `capture_wall.ps1` capture vs `opp_frame_709.bmp` (`C:\Users\Moinu\AppData\Local\Temp\opencode\`).

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

**Working on:** P3 residuals (§5.8 #10–#12 + §5.1/#5.6/#5.4/#5.3/#5.10 P3s). Unit 14 closed 2026-10-19 12:38; suite 791/791; runtime relinked 2026-10-19 12:30:26; code commit `472b661`.
**Handoff updated:** 2026-10-19 12:38 (+05:30) by DeepSeek V4.1 Flash Free.
