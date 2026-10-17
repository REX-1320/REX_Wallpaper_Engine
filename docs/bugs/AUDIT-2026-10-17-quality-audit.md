# AUDIT-2026-10-17 — full-repo code quality audit ("shape & sound")

> Method: 10 read-only `explore` agents (tiered scope: T1 wallpaper stack deep, T2 surface,
> T3 skim), every finding verified against cited `file:line` by the agent that reported it,
> then consolidated here. **No code was changed by this audit.** Companion bug-hunt audit:
> [`AUDIT-2026-10-06-parallel-audit.md`](AUDIT-2026-10-06-parallel-audit.md) (A1/A2/A3 re-verified below).
>
> **Counts:** 2× P0 · 14× P1 · ~30× P2 · ~55× P3/DOCS/TEST-GAP across 10 modules.
> **Prime user-visible tie-in:** the *intermittent first-seconds stutter* on the AV1 4K files
> (Aari, Mind Blowing OLED) matches the decoder EAGAIN retry-state defect (D1 below) — the
> decoder is heaviest exactly at startup (decode-ahead refill burst), and the defect only fires
> when the retry budget saturates, which is intermittent by nature.

Legend: `type`: BUG / SMELL / TEST-GAP / DOCS. Severity P0 (crash/corruption/user-blocking) →
P3. Confidence from the reporting agent (high = verified by reading).

---

## 1. Executive summary — the actionable top list

| # | Sev | Module | One-line | Evidence |
|---|-----|--------|----------|----------|
| A1 | **P0** | Audio | `ResizeRing` guard is always true (caller pre-assigns `m_channels`) → ring never resized; 4/6-ch endpoint ⇒ heap OOB write/read | `AudioPlayer.cpp:456,744`, consequence `:936-946,1029-1040` |
| Q1 | **P0** | App | `wallpaper:update` handler reads undeclared `audioMode` → `ReferenceError` on **every** live update; user gets an error toast on each settings change; audio-mode IPC unreachable | `app/electron/main.cjs:417,428` (introduced `8e13cd90`) |
| D1 | **P1** | Decoder | send-EAGAIN retry budget never resets after exhaustion → every later EAGAIN packet is dropped instantly (prime suspect for intermittent start stutter) | `FfmpegVideoDecoder.cpp:966-977` (+`hpp:409-413`) |
| E1 | **P1** | Engine | Recovery request consumed during backoff → a device-loss retry can be dropped permanently (A2 residual) | `Engine.cpp:1836-1868` |
| P1 | **P1** | Playback | `VideoManager::NotifyFrameConsumed` + `SetAudioOutputMode` iterate `m_monitorEntries` unlocked (UAF vs scene switch) | `VideoManager.hpp:101-108,227-232` |
| P2 | **P1** | Playback | Controller `NotifyFrameConsumed` reads `m_decoder` shared_ptr unlocked | `VideoPlaybackController.hpp:133-136` |
| W1 | **P1** | Platform | `ReacquireWorkerW` timeout path captures local `workerW` by reference into a queued window-thread task (use-after-scope) | `WorkerWManager.cpp:647` + `DesktopWindowThread.cpp:86-88,165-174` |
| G1 | **P1** | Graphics | `TextureCache` returns raw element pointers past its mutex; hot-reload thread rewrites them (live race on static-image path) | `TextureCache.cpp:73-89,55` |
| G2 | **P1** | Graphics | Compositor D3D12 init ignores every HRESULT → null-deref on any failure | `compositor.cpp:92-95,111-124,341-372` |
| R1 | **P1** | Renderer | HDR format-switch calls `ResizeBuffers` with back-buffer references held, no GPU drain (state desync on failure) | `SwapChain.cpp:330` via `D3D12Renderer.cpp:1401-1408` |
| R2 | **P1** | Renderer | `CommandQueue::m_fenceValue++` non-atomic; `WaitForGPU` shares one auto-reset event across waiters (one queue shared by multiple renderer instances) | `CommandQueue.cpp:80,87-91,101` |
| R3 | **P1** | Renderer | Static-image upload passes unaligned `RowPitch` (width×4) into D3D12 placed footprint (needs 256) — widths like 1366/3440 corrupt/fail | `D3D12Renderer.cpp:1525`; `WICTextureLoader.cpp:118` |
| AU2 | **P1** | Audio | `AudioCapture`: `WAVE_FORMAT_EXTENSIBLE` treated as int16 (typical shared-mode mix = 32-bit float → garbage spectrum); COM leaks on Start failure paths | `AudioCapture.cpp:149-166,24-84` |
| Q2 | **P1** | App | Audio mode never applied/reapplied (LOAD omits it, `settingsKey` ignores it, reapply drops it) | `app/electron/main.cjs:390-398,125-133`; `DesktopApply.jsx:35-43` |
| F1 | P2→(P1 link) | Decoder | Pool rebuild closes/recreates NT handles inside one Open — renderer's handle-keyed cache (8 slots, no eviction) permanently exhausted after a mid-stream rebuild | `FfmpegVideoDecoder.cpp:1147-1152` vs `D3D12Renderer.cpp:706-708,717` |

---

## 2. Tooling census (quality infrastructure)

| Gate | State | Evidence |
|------|-------|----------|
| `.clang-format` | **MISSING** | no file anywhere (root or `rex-core/`) |
| `.clang-tidy` config | **MISSING** | — |
| clang-tidy executable | present | `…\VS\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe` |
| clang-tidy sweep this audit | **NOT RUN — blocked** | VS generator can't emit `compile_commands.json`; a fresh Ninja configure fails because `C:/vcpkg/scripts/buildsystems/vcpkg.cmake` is a **STUB** ("Set up real vcpkg before reconfiguring"). Fresh configures are impossible until vcpkg is restored. |
| MSVC warning level | **effectively default** | `CMakeCache.txt`: `CMAKE_CXX_FLAGS:STRING=/DWIN32 /D_WINDOWS /EHsc` — no `/W3`, no `/W4`. A `/W4` census was not possible (same configure blocker). |
| CI `ci.yml` core job | builds **Debug** + `ctest` ✓ | `.github/workflows/ci.yml:21-28` |
| CI "Lint (clang-tidy)" step | **NO-OP** | runs `cmake --build build --target clang-tidy` — **no such target exists** in `rex-core/CMakeLists.txt` — and is `continue-on-error: true` (`ci.yml:30-32`). Lint has never run in CI. |
| CI `dotnet format` | `continue-on-error: true` | `ci.yml:60-62` |
| oxlint | present, minimal | `app/.oxlintrc.json` — 2 explicit rules (defaults apply); `pnpm lint` runs it |
| tsc | **strict ✓** | root `tsconfig.json`: `strict`, `noImplicitAny`, `noUncheckedIndexedAccess`, `noFallthroughCasesInSwitch` |
| TODO/FIXME/HACK comments | 0 across `rex-core/src`, `app/**` — anomalous for ≈40 K LOC; verify policy vs. reality (findings were concentrated in real code, not "noted" ones) |

**Recommendation (deferred to a tooling work unit, per user: no code edits now):** restore real
vcpkg (or vendor a minimal toolchain stub that doesn't block), add `.clang-format` + `.clang-tidy`,
give CI a real clang-tidy invocation (Fix: define the target or call clang-tidy directly once
`compile_commands.json` is producible), run a `/W4` census, then ratchet.

---

## 3. Cross-cutting status

### 3.1 Prior audit A1/A2/A3 (2026-10-06) — re-verified today

- **A1 telemetry UAF — FIXED.** `GetTelemetry` locks `m_mutex` (`Engine.cpp:1161-1164`);
  `ReinitializeRenderer` holds it across teardown/rebuild (`:295-349`); runtime joins telemetry +
  frame-poll threads before `engine->Shutdown()` (`wallpaper_cli_main.cpp:819-827,1378-1387`).
  The hotfix credit originally attributed to the joins alone was **misattributed** — the engine
  lock is the actual fix (the joins only cover shutdown). **One residual:** `SetProperty("audioMode")`
  dereferences `m_videoManager` unlocked (`Engine.cpp:1069-1075`).
- **A2 device-loss hot-spin/brick — PARTIAL.** Hot-spin gone (paced retry, `Engine.cpp:1866-1868`;
  backoff ladder `RecoveryBackoff.hpp:24-30`; loss flag cleared in `D3D12Renderer::Shutdown()`).
  **Still present:** the re-armed recovery request is consumed by `exchange(false)` (`:1836`) before
  the backoff delay elapses, and after one failed attempt a device-loss-only event is not re-armed
  → can still brick until restart for TDR-after-failed-re-init. Docs claiming full closure
  (TODO.md:154, everything.md:142) are overstated.
- **A3 SDR-on-HDR brightness — FIXED.** Shader normalizes by paper white (`D3D12Renderer.cpp:145-158`),
  SDR input scaled by `sdrWhiteNits` (`:291-292`); the 1000-nit masterMax divisor is gone.

### 3.2 Threading inventory (from module audits)

- Engine: render loop (owns `m_stopwatch` reset/read), command thread (IPC), telemetry thread
  (locks engine mutex), audio thread, decode thread per controller, desktop monitor/repair thread(s),
  watchdog thread (runtime). Found races: engine clock (`Engine.cpp:1475-1476,1782-1787` vs `716-720`),
  counters read by `DumpStats` (`:1442-1446`), unlocked getters `GetCurrentScene/GetCurrentMonitor`
  (`Engine.hpp:39,50`), `UnloadScene` vs render `Tick` pointer (`:1897-1901,2026-2032`), `Play/Stop`
  without state guards (`:668-694`), `Shutdown` re-entrancy (`:94-98,224-282`) — all reported.
- VideoManager/controller: render-thread `NotifyFrameConsumed` vs command-thread teardown (P1s above);
  `Shutdown` takes no lock (`VideoManager.cpp:80-106`).
- Decoder: pool vectors read mutex-free by `GetSlotHandle/GetSlotFence` (`FfmpegVideoDecoder.cpp:1045-1055`)
  while decode thread can rebuild pool; `static bool s_loggedFormat` process-global (`:1259`).
- Platform: `ProgmanHost` monitor thread mutates `m_renderWindow/m_progman/m_acquired` with no mutex
  (`ProgmanHost.cpp:448-451`); `WorkerWManager` monitor path reads fields without `m_opMutex`
  (`WorkerWManager.cpp:776-782`); `DesktopWindowThread::Start()` on a joinable thread → `std::terminate`
  (`DesktopWindowThread.cpp:34-35`).
- Compositor/renderer: no fence on the main render queue (`compositor.cpp:454-463`), unbounded
  `WaitForSingleObject(INFINITE)` (`:727-731,884-887`), CommandQueue fence counter (R2).

### 3.3 Test coverage map (highlights; full table in §5.10)

- Strong: scheduling pure functions (`test_frame_scheduler*`, `test_cadence_controller`, servo),
  decoder pure helpers, audio resample/downmix helpers, engine lifecycle/policies, settings,
  notification/command/search feature cores (~110–82 tests each).
- Blind spots (would have caught reported bugs): recovery state machine integration (E1),
  EAGAIN budget spanning packets (D1), ring-resize/format-parse in audio (A1/AU2), `FrameScheduler::Clear`
  vs concurrent `Tick` (P: FS05/06 are single-threaded), `NotifyFrameConsumed` concurrency,
  D3D12 runtime paths (no test constructs `D3D12Renderer`), `TextureCache`/`Compositor` D3D12 paths,
  module glue, `ModuleOrchestrator`, Electron IPC handlers (Q1 would be caught by one handler test).
- Env-gated tests silently `GTEST_SKIP` without media/D3D12 (`test_video_pipeline.cpp:24-31`) — CI can
  go green with **zero decoder execution**.
- Unbuilt test files: `tests/test_compositor_render.cpp`, `tests/test_workerw.cpp` (legacy header,
  cannot link), `tests/test_startup.cpp`, `tests/test_settings.cpp` — none referenced in CMake.

---

## 4. Stutter tie-in (user report 2026-10-17: Aari + Mind Blowing OLED stutter *sometimes* in the first seconds)

Both files are **AV1 4K** — heaviest decode init + the only codecs that saturate the decoder at
startup (decode-ahead refill burst is by design most aggressive right after PLAY). Candidate
mechanisms ranked by audit evidence:

1. **D1 (decoder, P1, intermittent-by-construction):** once the EAGAIN retry budget is exhausted,
   the counter is never reset → every later packet that meets a saturated decoder is dropped
   instantly, until a non-EAGAIN send happens to reset it. Whether the budget saturates at startup
   depends on packet interleaving/system load — matches "sometimes, not every time".
2. **F1 (decoder, pool handle churn):** a mid-startup-adjacent pool rebuild + renderer handle cache
   exhaustion (8 max, no eviction) would produce exactly "start degrades, steady recovers".
3. **Environmental (previously characterized):** GPU clock ramp / system load at first apply —
   do not fix code for this; A/B after 1+2 with harness + user watch.

The report recommends **Batch 1 = A1 + Q1 + D1 + AU2 (+ E1/W1 if approved)** then a user re-watch on
Aari/OLED before touching anything else.

---

## 5. Per-module findings

### 5.1 Engine (`rex-core/src/wallpaper/engine/`)

Verdict: substantially hardened since 2026-10-06 (locking on recovery swap, paced recovery,
telemetry teardown ordered), but the recovery retry state machine has a logic defect and several
render/command-thread data races remain.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | BUG | P1 | `Engine.cpp:1836-1868` | Recovery request consumed during backoff → device-loss retry lost after one failed attempt | `m_recoveryNeeded.exchange(false)` then attempt only `if (now >= m_nextRecoveryAttempt)`; failure re-arms but next pass (~16 ms, < 250 ms delay) consumes it and does nothing | Don't consume the flag when not due, or keep a persistent pending flag cleared only on success/Unload | high |
| 2 | BUG | P2 | `Engine.cpp:1069-1075` | `SetProperty("audioMode")` dereferences `m_videoManager` outside `m_mutex` (A1-class window) | lock released at 1072, call at 1073-1075 | snapshot `shared_ptr`/hold mutex | high |
| 3 | BUG | P2 | `Engine.cpp:1475-1476,1782-1787,1964-1969,1978-1979` vs `716-720,754-776` | `m_stopwatch`/`m_currentPosition` race: render thread resets/reads non-atomic `Stopwatch` while command thread uses it under lock | `m_stopwatch.Reset()` on render path; `GetPosition` under lock | render-thread-owned clock with atomic snapshot | high |
| 4 | BUG | P2 | `Engine.hpp:39,50`; `Engine.cpp:1569-1572,463,646` | `GetCurrentScene/GetCurrentMonitor` return by value unlocked while writers mutate | `SceneInfo GetCurrentScene() const { return m_currentScene; }` | lock/copy under mutex | high |
| 5 | TEST-GAP | P2 | `tests/wallpaper/test_engine.cpp:501-524` | Zero tests for recovery state machine / retry integration | only pure delay ladder tested | fault-injection seam + concurrent telemetry test | high |
| 6 | BUG | P3 | `Engine.cpp:1897-1901,2026-2032` vs `628-631` | `UnloadScene`/`Stop` can close controller + clear frames while render thread holds a `Tick` pointer | render `pending = m_videoManager->Tick(...)` … `DrawVideoFrame(*pending)`; command `Close(); ClearVideoFrames();` | gate teardown on render acknowledgment or re-order state | med |
| 7 | BUG | P3 | `Engine.cpp:94-98,224-282` | `Shutdown()` lacks an already-shutting-down guard; second concurrent caller proceeds | `ShuttingDown` falls through | early-return on `ShuttingDown`; guard dtor | med |
| 8 | BUG | P3 | `Engine.cpp:668-694,729-752` | `Play()`/`Stop()` lack state guards (Play during Error/recovery) | Play only checks `sourcePath.empty()` | documented transition table | med |
| 9 | BUG | P3 | `Engine.cpp:1442-1446` vs `1466-1467,1844-1845` | `DumpStats` reads render-thread counters unsynchronized | `m_frameCount`, `m_frameTime`, `m_recoveryCount`… | atomics or snapshots | high |
| 10 | ROBUST | P3 | `Engine.cpp:850-853` | `EnumerateMonitors` ignores `WideCharToMultiByte` result; possible unterminated copy | `char nameBuf[128]; … info.id = nameBuf;` | check return size | med |
| 11 | SMELL | P3 | `Engine.cpp:32-61,883,1425,1485-1506` | DXGI factory created per monitor per call on housekeeping/telemetry paths | `CreateDXGIFactory1` inside per-monitor helper; self-logged "slow housekeeping" | query once, cache in `MonitorInfo` | high |
| 12 | SMELL | P3 | `Engine.hpp:149-150,213-218`; `Engine.cpp:1392` | Dead state: `m_lastPrimaryWidth/Height` write-only; `m_pressureScale` never ≠1; unused constants | grep evidence | remove or document | high |
| 13 | DOCS | P3 | `Engine.hpp:89-90`; `CadencePolicy.hpp:20` | Comment claims PaceLoop floor ~15 Hz; actual floor 4 ms (250 Hz) | `std::max(1000.0 / fps, 4.0)` | fix comment or implement | high |
| 14 | SMELL | P3 | `Engine.cpp:226-248,295-357` | `m_mutex` held across heavyweight recovery work and `StopMonitor()` join (up to 3 s stalls every command API) | `lock_guard` spans `StopMonitor()`/`Initialize()` | narrow lock: swap under lock, init outside | high |
| 15 | SMELL | P3 | `Engine.cpp:69-88` vs `1251-1302` | GPU video-memory query duplicated (helper vs inline telemetry), two error styles | — | extract one helper | high |

### 5.2 Decoder (`rex-core/src/wallpaper/decoder/`)

Verdict: carefully engineered (good "why" comments, atomics where it matters); but two P1-class
defects sit directly on the stutter-critical path, plus error-path holes and lying diagnostics.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | BUG | P1 | `FfmpegVideoDecoder.cpp:966-977` (+`hpp:409-413`) | send-EAGAIN retry counter never reset when budget exhausted → all later packets get zero retries and are dropped instantly | `} else { m_sendEagainRetries = 0; }` runs only for non-EAGAIN; after a stall `ShouldRetry(8, 8)` = false at 966, drop at 978-979 | reset on exhaustion or make per-packet local | high |
| 2 | BUG | P2 (P1 impact) | `FfmpegVideoDecoder.cpp:1142-1158,1195-1197,1239-1242` | Pool marked built before all textures exist; partial failure returns true forever (null texture slot) | assigns `m_poolW/H/Format` before creation loop; `return false` mid-loop; fast path skips re-creation | build into temporaries, commit after full success | high |
| 3 | BUG | P2 | `FfmpegVideoDecoder.cpp:1147-1152,1198-1202` vs `D3D12Renderer.cpp:706-708,717` | Pool rebuild recreates NT handles inside one Open → renderer handle cache (8 max, no eviction) can be permanently exhausted | `sharedHandle` documented "stable per Open" but cleared/resized | stable handles (double-buffered pool generations) or publish generation for eviction | med-high |
| 4 | BUG | P2 | `FfmpegVideoDecoder.cpp:1284` vs `1392-1393` (`hpp:470-487`) | SW path bypasses slot leasing: blind round-robin, never stamps `m_slotFrameIndex` | `const UINT slot = m_poolNext; …` vs HW `PickPoolSlot()` + stamp | route SW through `PickPoolSlot()` | high |
| 5 | BUG | P2 | `FfmpegVideoDecoder.cpp:506,1296-1298,1425-1427` + `VideoManager.cpp:128-129,539-542` + `D3D12Renderer.cpp:843-845` | Fence values restart at 1 per decoder Open while one shared fence is used by all decoders; a consumer can be released by an older producer's higher value | `Close()` resets `m_fenceValue = 0`; renderer waits `WaitForFence(fenceValue)` | process-global monotonic counter or per-session fence | med |
| 6 | BUG | P2 | `FfmpegVideoDecoder.cpp:146-161` | Partial QueryInterface failure leaks AddRef'd interfaces | four QIs into raw locals; early return before any `Attach` | ComPtr locals / attach immediately | med-high |
| 7 | BUG | P2 | `FfmpegVideoDecoder.cpp:1346-1354` (+`400-403`) | swscale keyed on `codecpar->format` instead of last-used source format → context recreated every frame when they differ; Open can build from `AV_PIX_FMT_NONE` | `frameFormat != static_cast<AVPixelFormat>(m_stream->codecpar->format)` | cache actual source format; validate != NONE | med |
| 8 | SMELL | P2 | `FfmpegVideoDecoder.cpp:1086-1088,1206-1226,1248,1331-1342` (+`hpp:84-93`) | Dead/dishonest diagnostics: `m_vpMsRing` never written yet vp P50/95/99 published; video processor created but no `VideoProcessorBlt` exists; hw-download branch unreachable; `Flush()` etc. no callers | `m_vpMsCount` only read; `m_processor` only created | delete or wire; zero the telemetry | high |
| 9 | BUG | P2 | `FfmpegVideoDecoder.cpp:966-979,1004-1006,1023` | EAGAIN-budget exhaustion counted as send failure; can latch `unusableCode = 2` ("cannot decode") on a merely saturated decoder | stall path falls into `++m_sendFailStreak`; `neverDecoded` escalates store 2 | separate stalls from hard errors | med |
| 10 | BUG | P3 | `FfmpegVideoDecoder.cpp:1259-1277` | `static bool s_loggedFormat` process-global non-atomic across decoder instances (multi-monitor) | — | member / `std::call_once` | high |
| 11 | BUG | P3 | `FfmpegVideoDecoder.cpp:1045-1055` vs `1147-1153,492-496` | `GetSlotHandle/GetSlotFence` read pool vectors mutex-free while decode thread can clear/resize | `slot >= m_poolHandles.size()` then index | fixed array swapped atomically, or seqlock | med |
| 12 | SMELL | P3 | `FfmpegVideoDecoder.cpp:229-250 vs 666-684; 254-409 vs 594-691` | Open vs SelectStreamByResolution duplication; stream switch regresses SW decode to `thread_count = 1` (contradicts benchmarked rationale) | `607/625` force 1 | shared open helper; preserve auto threads | high |
| 13 | SMELL | P3 | `FfmpegVideoDecoder.cpp:785` | `defaultPeak` ternary identical in both branches (HLG vs PQ intent lost) | `HLG ? 1000.0 : 1000.0` | implement or collapse | high |
| 14 | BUG | P3 | `WICTextureLoader.cpp:115,118,121` | Unchecked `GetSize` HRESULT; 32-bit pitch/size arithmetic can overflow | `rowPitch * height` | check HR; 64-bit size; reject implausible dims | med |
| 15 | TEST-GAP | P3 | `test_pool_slot_lease.cpp:20-42; test_decoder_packet_contract.cpp:200; test_decoder_eof.cpp; test_multi_monitor.cpp:178-184` | Tests can't detect drift: `PickSlotOldestSafe` mirrored in test; pump resets retries per packet (hides D1); EofLoopGuard pure; dual-decoder test asserts mute policy only | file comment "Mirrors … exactly" | link real functions; add cross-packet EAGAIN test | high |

### 5.3 Audio (`rex-core/src/wallpaper/audio/`)

Verdict: decode/downmix/resample core is genuinely well-built (EAGAIN retry, wrap-aware ring,
endpoint re-enumeration, MMCSS, real unit tests for helpers). **But the channel-layout/ring-resize
path is broken (P0)** and `AudioCapture` is materially weaker.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | BUG | **P0** | `AudioPlayer.cpp:456,744` | `ResizeRing` dead code: caller sets `m_channels` first, equality guard always returns early; ring never resized after Open | caller `m_channels = ch; break;` then `ResizeRing(m_channels)`; guard `if (m_channels == channels …) return` | compare against ring's allocated channel count / pass old+new | high |
| 2 | BUG | P1 | `AudioPlayer.cpp:936-946,1029-1040` | Consequence of #1: `m_channels=6` + 2-ch ring → heap OOB write/read in ring copies | `ringBase + wp * ringCh` on a vector sized `m_ringFrames*2` | fix #1; assert size invariant | high |
| 3 | BUG | P1 | `AudioCapture.cpp:149-166` | `WAVE_FORMAT_EXTENSIBLE` treated as int16 without checking `SubFormat`/`wBitsPerSample`; typical shared-mode mix is 32-bit float → garbage spectrum | `} else if (… PCM || … EXTENSIBLE) { const int16_t* s = …` | check SubFormat GUID + bit depth | high |
| 4 | BUG | P1 | `AudioCapture.cpp:24-84,94-95,103` | Start failure paths leak: CoInitializeEx ref never released; `m_waveFormat` freed only in Stop which early-returns when not running; engine retries Start per audio scene → COM refcount accumulation | `if (!m_running.load()) return;` | track `m_comInitialized`; free on every failure exit | high |
| 5 | BUG | P2 | `AudioCapture.cpp:25,104` | `Stop()` calls `CoUninitialize()` unconditionally even after `RPC_E_CHANGED_MODE` (no ref added) → COM underflow | — | record S_OK/S_FALSE | high |
| 6 | BUG | P2 | `AudioCapture.cpp:24,87,116-138` | COM objects created on caller thread (STA) then used on `CaptureThread` with no CoInitializeEx/marshalling (AudioPlayer does this correctly) | contrast `AudioPlayer.cpp:496` | init COM (MTA) on capture thread or marshal | med |
| 7 | BUG | P2 | `AudioCapture.cpp:125-128` | No device-change recovery: `GetNextPacketSize` failure loops `Sleep(10)` forever; after default-device switch spectrum freezes silently | — | detect `…DEVICE_INVALIDATED`, re-enumerate | high |
| 8 | BUG | P2 | `AudioCapture.cpp:134-137,146-166` | `GetBuffer` flags ignored (`AUDCLNT_BUFFERFLAGS_SILENT`), `data` never null-checked before deref | — | treat SILENT as zeros; bail if null | med |
| 9 | BUG | P2 | `AudioPlayer.hpp:85,167,189-190` vs `.cpp:456,745` | Public getters return non-atomic `m_channels/m_sampleRate/m_hasAudio` while audio thread writes them (header advertises thread-safe control surface); latent (no in-tree callers) | — | atomics or locked snapshot | high |
| 10 | BUG | P2 | `AudioPlayer.cpp:797-808` | `DoSeek` ignores `avformat_seek_file` result, then flushes/clears ring — failed seek indistinguishable from success (A/V desync); `RewindToStart` checks correctly | — | check return; keep state on failure | high |
| 11 | SMELL | P2 | `AudioPlayer.cpp:990-1017` | Render callback allocates `std::vector` + runs resampler under ring mutex (MMCSS thread) | `std::vector<int16_t> content(…)` inside `lock_guard` | reusable scratch; shorten critical section | high |
| 12 | TEST-GAP | P2 | `test_audio_resample.cpp`, `test_audio_dolby.cpp` | Only pure helpers tested; zero coverage for `AudioCapture` (incl. format parse), ring wrap, `ResizeRing`, endpoint loss, COM balance | — | extract/test ring + parse logic (would catch #1/#3) | high |
| 13 | BUG | P3 | `AudioPlayer.cpp:396-405` | `PROPVARIANT` leak unless `VT_LPWSTR` + non-null; any other successful type leaks per endpoint open | `if (SUCCEEDED(GetValue) && name.vt == VT_LPWSTR && name.pwszVal)` | clear whenever GetValue succeeded | med |
| 14 | BUG | P3 | `AudioPlayer.cpp:1059` | `ReleaseBuffer` return ignored; endpoint-loss HRESULT dropped (detected one buffer later only) | — | check HR → flag endpoint loss | med |
| 15 | BUG | P3 | `AudioPlayer.cpp:496,638` | `CoInitializeEx` result ignored but `CoUninitialize` unconditional | — | track and pair | med |

Notes: the EXTENSIBLE-as-int16 defect is **duplicated verbatim** in the orphaned
`wallpaper/renderer/AudioVisualizerRenderer.cpp:414-424` (fix only if that tree is revived);
dead code: `m_bytesPerSample`, `AudioCapture::m_device` (never assigned), stale `m_ringMutex`
comment; near-identical wrap-aware copy loops duplicated in `PushFrameToRing`/`RenderStep`.

### 5.4 Playback & video (`rex-core/src/wallpaper/playback/` + `video/`)

Verdict: core scheduling design (SPSC ring, PTS-due selection, reserve-scaled pacing, servo) is
unusually well-reasoned and heavily tested on pure surfaces, but threading contracts at the seams
are overstated; scheduler `Clear()` is not safe vs concurrent `Tick()`.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | BUG | P1 | `VideoManager.hpp:101-106` | `NotifyFrameConsumed` iterates `m_monitorEntries` unlocked; render thread races command-thread `ClearVideoFrames` (UAF) | loop at hpp:103; `ClearVideoFrames` resets under lock at cpp:521; callers `Engine.cpp:2086` vs `513/541/630` | take `m_monitorMutex` or snapshot | high |
| 2 | BUG | P1 | `VideoPlaybackController.hpp:133-136` | `NotifyFrameConsumed` reads `m_decoder` unlocked while `Close()` resets it (UAF; violates P1-8 snapshot discipline) | `if (m_decoder) m_decoder->…` vs `m_decoder.reset()` at cpp:201 | use `SnapshotDecoder()` | high |
| 3 | BUG | P2 | `FfmpegVideoDecoder.cpp:1392-1393` | SW-decode path bypasses pool-slot lease (cross-listed with decoder #4) | — | route through `PickPoolSlot()` | high |
| 4 | BUG | P2 | `FrameScheduler.hpp:67-72` / `cpp:145-154` | `Clear()` not safe vs concurrent `Tick()`: lost update un-clears ring + re-arms burst gate | Tick stores after Clear stores; header claims any-thread safe | epoch-aware Clear or route via consumer thread | high |
| 5 | BUG | P2 | `VideoPlaybackController.cpp:215` | `Play()` cannot restart a non-looping video after EOF: gate compares to `duration-0.001`, last PTS is one frame short | cpp:215; decoder EOF `903-914`; `m_playing=false` cpp:621 | explicit `m_eosReached` flag or `duration - frameInterval` | high |
| 6 | BUG | P2 | `VideoManager.cpp:227-232` | `SetAudioOutputMode` iterates entries unlocked | cpp:229; mutators AddMonitor cpp:275/305, RemoveMonitor 340 | same as #1 | high |
| 7 | BUG | P2 | `VideoManager.cpp:80-106` | `Shutdown` takes no lock, resets `m_controller`, doesn't tear down entries; class doc claims any-thread safety | cpp:83 vs `Tick` deref cpp:246-248; host-ordering mitigates | make self-contained | med |
| 8 | BUG (latent) | P2 | `VideoManager.cpp:375` | Per-monitor video `Tick(0.0)` hardcoded target; can never advance past first frames under strict retain policy; latent (no production callers) | cpp:375; `WrapTarget` hpp:201-203; selection `FrameScheduler.cpp:76` | pass wall-clock position | high |
| 9 | DOCS+SMELL | P3 | `FrameScheduler.hpp:76-82,130-133` / `cpp:144` | "Monotonic presentation guard" dead code; unsynchronized plain double; three stale comments | `m_lastPresentedPts` written cpp:144, read only in debug log | delete or make real | high |
| 10 | TEST-GAP | P2 | `tests/wallpaper/test_frame_scheduler.cpp:76-105` | No concurrency coverage for the above; FS05 Push+Tick only; lease test mirrors production fn; multi-monitor test asserts mute only | — | interleave tests; link real functions | high |
| 11 | SMELL | P3 | `VideoManager.cpp:597-604` | `StopMonitor` calls `Close()` not `Stop()` — monitor video cannot be resumed/queried after stop | cpp:601 vs main controller semantics | `Stop()` or rename | high |
| 12 | SMELL | P3 | `VideoPlaybackController.cpp:723` | Decode thread reads `m_decoder` shared_ptr without leaf mutex (safe only because Close joins first) | cpp:723 vs `SnapshotDecoder` pattern | use SnapshotDecoder | med |
| 13 | SMELL | P3 | `VideoPlaybackController.cpp:359-374` | Adaptive stream switch refreshes fps but never re-scales decode-ahead policy | `m_decodeAheadPolicy = PolicyFor(m_frameRate)` only in Open cpp:65 | recompute after stream selection | med |
| 14 | SMELL | P3 | `FrameScheduler.hpp:33-39` / `cpp:22` | SPSC "always distinct indices" claim false against producer lapping; returned pointer can be overwritten | Push overwrites oldest slot | document semantics / gate on consumed watermark | med |
| 15 | DOCS | P3 | `test_frame_scheduler.cpp:274`; `CadenceController.hpp:83,136` | Stale test name (`FS15_FullRingOfFutureFallsBackToNewest` asserts retention); catch-up default wording contradicts lines 57/87 | — | rename/fix docs | high |

### 5.5 Runtime & protocol (`rex-core/src/wallpaper/runtime/` + `wallpaper_cli_main.cpp`)

Verdict: functional but showing age; A1 genuinely fixed (engine-side lock is the real fix — §3.1);
two P2 liveness hazards in the shipped quit path.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | lifecycle | P2 | `wallpaper_cli_main.cpp:662` | Telemetry thread sleeps 5 s unsliced; SHUTDOWN join can exceed Electron's 3 s kill | `sleep_for(seconds(5))`; join :819-820; client kills at 3000 ms (`wallpaperRuntime.cjs:219-226`) | slice sleep in 250 ms chunks | high |
| 2 | liveness | P2 | `wallpaper_cli_main.cpp:882-891` | Watchdog "never blocks on engine" but `GetTelemetry` takes engine mutex (held across full device recreation) | lambda calls `engine->GetTelemetry`; `ReinitializeRenderer` holds `m_mutex` (Engine.cpp:295-365) | lock-free snapshot or try_lock | high |
| 3 | robustness | P2 | `wallpaper_cli_main.cpp:542-554` | Oversized length prefix leaves body unread → permanent stream desync on that session | `ReadFrame` consumes 4 bytes, answers, continues; `FrameProtocol.hpp:3-8` claim only true for len==0 | drain `min(len,cap)` or tear down session | high |
| 4 | robustness | P3 | `wallpaper_cli_main.cpp:1070-1100` | SET_* with missing `value` silently applies 0/false (mutes/zeroes instead of erroring) | `AsFloat(0.0f)`, `AsBool(false)` | require field | high |
| 5 | correctness | P3 | `runtime/Json.hpp:67` | `AsInt/AsFloat` UB on out-of-range (`1e999`→inf→`static_cast<int>` UB); reachable via `id`, `maxFps`, `pid`, SET_* values | — | reject non-finite/out-of-range | high |
| 6 | correctness | P3 | `wallpaper_cli_main.cpp:1219-1247` | GET_FRAME_SURFACE: `slotCount` unclamped; no duplicated-vs-local handle marker; latent (dev tool consumer) | slots clamped to 8, `s.slotCount` emitted | emit clamped count + `duplicated` flag | high |
| 7 | maintainability | P3 | `runtime/RendererAdapters.cpp:64-65 vs 76-81` | Comment says volume/mute unsupported; code reports them supported | — | fix comment/entries | high |
| 8 | maintainability | P3 | `runtime/RuntimeSettings.cpp:20-23,34-51,67-71` | Silent save failures; unbounded load; used only by never-constructed `PlaylistManager` (runtime persists via `SettingsService`) | — | propagate errors; cap; delete unused path | high |
| 9 | robustness | P3 | `wallpaper_cli_main.cpp:114-115 vs 147-157` | Settings persisted cwd-relative while logs go to `%TEMP%\REX`; Electron spawns with no cwd → dev vs packaged persist to different dirs | `ConfigurationStore("config/settings.json")` | absolute per-user path | med |
| 10 | robustness | P3 | `wallpaper_cli_main.cpp:75-81` | `WidenToUtf8` lacks the `size <= 0` guard its sibling has | vs `:91` (P2-17 fix) | mirror guard | med |
| 11 | robustness | P3 | `wallpaper_cli_main.cpp:790-794` | Up-to-16 MiB malformed payload logged verbatim (log flood) | `REX_LOG_WARN(… frame.c_str())` | log length + prefix | high |
| 12 | maintainability | P3 | `wallpaper_cli_main.cpp:495-497` | Stale command list in header comment | missing PING + 5 SET_*/2 surface commands | regenerate | high |
| 13 | maintainability | P3 | `wallpaper_cli_main.cpp:739-1390` | `RunRuntime` ≈650 lines / 20+ commands; SHUTDOWN teardown duplicated at EOF; `RunBenchmark` ≈840 lines with 10× repeated cleanup | — | extract helpers; split phases | high |
| 14 | tests | P3 | `CMakeLists.txt:377-380,406-407` | Protocol behavior largely untested (`ReadFrame` partial/EOF/malformed, dispatch loop, SHUTDOWN join ordering not testable — live in `wmain`/statics) | `test_protocol_safety.cpp` covers 2 helpers only | move frame-read + dispatch into testable TU | high |
| 15 | app-contract (handoff) | P2 | `app/electron/main.cjs:417,428` | `audioMode` ReferenceError (see §5.10 / Q1) | — | — | high |

Positively verified: partial reads (`:502-512`), zero/oversize classification (`:547-549`), JSON depth
cap + trailing garbage (`Json.hpp:218,121-128`), unknown commands answered, single-runtime guard
(`:750-756`), ClientSurface duplication/detach, lock-order (runtime guard → engine mutex, never reverse),
60-specifier telemetry fprintf checked against `EngineTelemetry`.

### 5.6 Renderer (`rex-core/src/graphics/renderer/` — live path)

Verdict: broadly sound (fenced frame-context reuse, dedicated wait events, occlusion handling) but
two P1 defects sit in real user paths plus a structural queue-sharing hazard. **Also discovered:
the directory literally named `wallpaper/renderer/` is orphaned, cannot compile, and is in no target
(§6).** All fixes read-only, colour freeze respected.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | BUG | P1 | `SwapChain.cpp:330` (via `D3D12Renderer.cpp:1401-1408`) | HDR format-switch `ResizeBuffers` with `m_renderTargets` never released, no `WaitForGPU`; failure branch desyncs state | contrast `Resize()` `SwapChain.cpp:121` + drain `D3D12Renderer.cpp:1386`; reached from `CheckDisplayChanges` | release refs + drain; don't mutate state on failure | high |
| 2 | BUG | P1 | `CommandQueue.cpp:80,87-91,101,112-114` | Non-atomic `m_fenceValue++`; `WaitForGPU` shares one auto-reset event (own comment admits lost wakeups); queue shared across renderer instances on different threads | `Engine.cpp:2344` → `MonitorCompositor.cpp:73-77` | atomic/mutex counter; per-wait events | high |
| 3 | BUG | P1 | `D3D12Renderer.cpp:1525` (+`1460,1495-1499`; `WICTextureLoader.cpp:118`) | Static-image upload RowPitch unaligned (width×4) for D3D12 placed footprint (needs 256) — 1366/3440-wide images | video path pads correctly `966-971` | pad rows to 256 | high |
| 4 | BUG | P2 | `D3D12Renderer.cpp:1424-1430` | HDR PSO creation failure logs "staying on SDR" but swapchain already R16F/PQ → PSO/RTV mismatch + wrong transform | — | don't clear flag unless swapchain reverted | high |
| 5 | BUG | P2 | `Device.cpp:34-38` | Adapter enum loop stops only on `DXGI_ERROR_NOT_FOUND`; other failures leave null `adapter` then deref | `adapter->GetDesc3` at :38 | check HRESULT each iteration | high |
| 6 | SMELL | P2 | `wallpaper/renderer/*` | Orphaned subsystem (not in CMake; calls nonexistent `WaitForExternalFence`) — misleads scoping | see §6 | delete or archive | high |
| 7 | BUG | P2 | `AudioVisualizerRenderer.cpp:161-162`; `RenderPipeline.cpp:171` | Latent bugs inside dead code (fence never incremented; no `OMSetRenderTargets`; RTV unbound before draws) | — | fold into #6 decision | high |
| 8 | TEST-GAP | P2 | `CMakeLists.txt:301,369-411` | No test constructs `D3D12Renderer`; coverage is header policies + smoke using an aligned-width jpg (hides #3) | `test_resource_leak.cpp:37-61` | offscreen device tests (misaligned upload, HDR toggle, queue concurrency) | high |
| 9 | SMELL | P3 | `D3D12Renderer.cpp:1259 vs 1666 vs 1818` | `cb[15]` written but only 15/16 floats uploaded; fallback equals hardwired default → future setter silently no-ops | — | raise `Num32BitValues` to 16 | high |
| 10 | SMELL | P3 | `D3D12Renderer.cpp:915,1097` | `UpdateTexturePixels` has zero in-tree callers; its per-frame `WaitForGPU` would serialize the shared queue if wired | — | delete or document | high |
| 11 | SMELL | P3 | `D3D12Renderer.cpp:322-459 vs 461-571` | ~200-line near-duplicate `Initialize` overloads; dead audio CB path; unused `DescriptorHeapManager` include | — | single init core | high |
| 12 | SMELL | P3 | `DisplayCadenceStats.hpp:94-95 vs 163-164` | `m_gapMicros` stores raw QPC ticks but getters claim micros (~2.4× off on non-10 MHz QPC); no consumers | own comment notes 24 MHz | convert or rename | high |
| 13 | SMELL | P3 | `D3D12Renderer.cpp:1171-1172,1296,1387`; `SwapChain.cpp:342` | Unchecked HRESULTs on hot paths (`Reset`, `Close`, `Resize`, `SetColorSpace1`) | — | check and log; escalate device-loss | high |

### 5.7 Core / platform / ipc / profiles / shared

Verdict: foundation happy paths sound (`PacingServo`, `PreciseSleep`, `LayerRepairPolicy` well-shaped
+ tested); real trouble in the desktop-host concurrency model and `ipc_server` handle lifetime;
a sizable dead-weight fraction.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | lifetime/UAF | P1 | `platform/windows/WorkerWManager.cpp:647` | Queued window-thread task captures local `workerW` by reference; runs after return on timeout | task may still be queued (`DesktopWindowThread.cpp:86-88`), drain runs late tasks (`:165-174`); safe pattern at `:134`/`ProgmanHost.cpp:143-144` | shared_ptr outcome | high |
| 2 | concurrency | P2 | `ipc/ipc_server.cpp:126-131` | Listener closes `pipe` before invalidating `m_pipe` under mutex → `Stop()/Send()` TOCTOU on closed/recycled handle | same at `:81-84,90-94`; contradicts sole-owner contract (`ipc_server.h:31-34`) | invalidate under lock first | high |
| 3 | thread start | P2 | `platform/windows/DesktopWindowThread.cpp:34-35` | `Start()` assigns `m_thread` while possibly joinable → `std::terminate` | callers re-invoke after failed start (`WorkerWManager.cpp:89-103`, `ProgmanHost.cpp:31-45`) | join-if-joinable before reassign | high |
| 4 | lifetime/UAF | P2 | `ipc/ipc_server.cpp:41-50` | `Stop()` from inside handler detaches; listener keeps using `this` after destruction | ListenLoop touches members after detach | shared ownership or signal completion | high |
| 5 | data race | P2 | `platform/windows/ProgmanHost.cpp:448-451` | Monitor thread mutates `m_renderWindow/m_progman/m_acquired` with no mutex; races `Release/Acquire/SetVisible` | no `m_opMutex` equivalent; pointless `const_cast` | port WorkerWManager pattern | high |
| 6 | data race | P2 | `platform/windows/WorkerWManager.cpp:776-782` | Monitor/repair path reads `m_acquired/m_renderWindow/m_workerW` without `m_opMutex` (writers hold it) | same for `TryReanchorLayer :270-273`, `SetVisible :252` | extend mutex or atomics | high |
| 7 | correctness | P2 | `profiles/services/ProfileManager.cpp:12-27,41-42` | Persistence drops `triggers` + timestamps; no escaping (values with `\n`/`=`/`|` corrupt); `evaluateTrigger` returns true for every non-Manual; callback unlocked | — | real format; implement/reject triggers; lock callback | high |
| 8 | correctness | P2 | `profiles/services/ProfileManager.cpp:174-231` | `applyProfile` never applies `wallpaperId` (or `themeId`); injected wallpaper engine unused | `m_wallpaperEngine` only in header/ctor | wire or drop dependency | high |
| 9 | robustness | P3 | `ipc/ipc_server.cpp:14,135-147` | Double `Start()` terminates; `Send()` >4 KiB silently fails; all read errors treated as disconnect | `PIPE_BUFFER_SIZE` 4096; `ERROR_MORE_DATA` → generic break | guard; report/limit; document | high |
| 10 | resource | P3 | `core/sandbox/sandbox.cpp:7-31,33-66` | Repeated `Initialize/SpawnProcess` leak handles; unchecked `ResumeThread`; unused `SECURITY_ATTRIBUTES` | — | reset state at entry; check; drop dead `sa` | high |
| 11 | dead code | P3 | `platform/windows/Windows.cpp:6-35` + `render_scheduler.*` + `shared/unique_handle.hpp` | Whole modules compiled but unreferenced; stubs return misleading values | greps; see §6 | delete or quarantine | high |
| 12 | robustness | P3 | `platform/windows/MonitorEdid.cpp:39-45` | EDID registry read no value-type check; no tests; live engine dependency (`Engine.cpp:876`) | discards `REG_BINARY` type | validate type; add test | high |

### 5.8 Graphics (non-renderer rest)

Verdict: small but with two real defects; roughly half the module (animation/effects) compiled and
referenced by nothing; only 5 CPU-only Compositor tests.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | data race/UAF | P1 | `graphics/resource/TextureCache.cpp:73-89,55` | Mutex-protected cache returns raw pointers that escape the lock; hot-reload thread rewrites entries; `Evict/Clear` dangle live pointers | `GetOrLoad/Reload/Get` return `&it->second.texture` after unlock; `ImageLoader` hot-reload `:117-127` vs `StaticImageScene.hpp:105-106` | shared ownership or generation handles | high |
| 2 | crash/HRESULT | P1 | `graphics/compositor/compositor.cpp:92-95,111-124,136,341-372` | Init ignores every HRESULT then dereferences possibly-null objects; still returns true | — | check all; guard; set initialized only on success | high |
| 3 | sync | P2 | `compositor.cpp:454-463,54-80` | No fence on main render queue: allocator reset/shutdown races in-flight work; no device-removed path | `m_commandAllocator->Reset()`; `Present` result only logged | per-frame fence; check DEVICE_REMOVED | high |
| 4 | barrier | P2 | `compositor.cpp:582-619,706-716` | `UpdateLayerPixels` repeats transitions with wrong `StateBefore` on second call (texture already SRV) | barrier claims `COPY_DEST` | track resource states | high |
| 5 | hang | P2 | `compositor.cpp:727-731,884-887` | Unbounded `WaitForSingleObject(INFINITE)` on fences, no device-removed/timeout path | — | bounded wait + device check | high |
| 6 | leak | P2 | `compositor.cpp:539-542,903-909` | SRV slot leaked when a texture replaces an existing layer id (exhausts 64) | — | free/reuse slot before overwrite | high |
| 7 | format | P2 | `compositor.cpp:502-537` | `ImportLayerTexture` forces R8G8B8A8 SRV for any shared handle; ignores caller width/height; unchecked view creation | — | derive from `GetDesc()`; validate | med-high |
| 8 | sync | P2 | `compositor.cpp:519-542,435-441` | Shared-handle import has no producer/consumer fence or acquire transition | — | shared-fence sync or documented ownership | med |
| 9 | state | P2 | `compositor.cpp:377-383,463-478` | Back buffer assumed PRESENT; failed Present breaks next frame's transition | device-removed case definite | track per-backbuffer state | med-high |
| 10 | logic | P3 | `graphics/animation/animation_engine.cpp:18-40` | Mutation during iteration; >64 completions deferred; `durationMs == 0` → NaN, never completes; no consumers | — | collect then invoke; spec zero-duration | high/low |
| 11 | debug artifact | P3 | `compositor.cpp:736-739` | Hardcoded `C:\Users\munna\...\rex_bg_log.txt` compiled into production `SetBackgroundFromFile` (~15 call sites) | — | remove / project logger | high |
| 12 | contract | P3 | `graphics/effects/effect_renderer.cpp:52-56,74-76` | Backdrop renderers report capabilities inconsistent with what they apply; no OS build check | — | reject unsupported types | high/low |

### 5.9 Module platform (`modules/` + `desktop_personalization/`) — surface scan

Verdict: module kernel (registry/loader/deps/lifecycle/health) is the strongest part; the platform
layers above are islands — **nothing drives `update()` and no module starts on a fresh boot**
(`main.cpp` bare `GetMessage` loop; `app/` references no module IDs).

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | architecture | P2 | `main.cpp:44-54` | No driver: modules never start or tick in the shipping binary (first run: `config/modules.json` absent → all stay Initialized; `update()` never called) | — | update pump + explicit start, or IPC surface | high |
| 2 | deadlock | P2 (P1 once wired) | `modules/Notification/services/NotificationManager.cpp:61,75-80,191` | `dismissNotification` self-deadlocks: holds non-recursive mutex, renderer invokes callback synchronously which re-locks | real renderer installed `NotificationModule.cpp:27,34`; tests use deferring spy | invoke callback outside lock | high |
| 3 | dead architecture | P2 | `main.cpp:40-42` vs `desktop_personalization/DesktopPersonalizationModule.cpp:80-82` | Two parallel personalization stacks; richer one never constructed; duplicated registry helpers/keys | greps | pick one | high |
| 4 | stub/misleading | P2 | `modules/Personalization/platform/WinStartMenuAdapter.cpp:18-28,18-28` (+WinExplorerAdapter) | Adapters are placeholders claiming complete (`apply()` writes invented values; `#project_status.md:223` says ✅) | `Start_ShowRecommended`; `compactMode`→`DisallowShaking` | implement or mark 🟡 | high |
| 5 | shell mutation | P2 if wired | `desktop_personalization/platform/Win32PlatformAdapter.cpp:109,117,126,201-242` | Backdrop calls no-op (`DwmSetWindowAttribute(nullptr,...)`); `restartExplorer` blunt `taskkill /f` unchecked, no wait; wrong reg types | — | real HWND; wait; types | high |
| 6 | dead/crypto stub | P3 | `modules/Sync/crypto/CryptoService.cpp:12,16-23` (+`SyncRestClient.cpp:12-16`) | Sync stack compiled into binaries, fake (XOR with `mt19937(42)` "encryption"; mock login success), unreachable | — | exclude or complete+gate | high |
| 7 | dead code | P3 | `modules/Widgets/services/WidgetRuntime.cpp:7-37` | Widget runtime never constructed; HTML expects `window.chrome.webview` with no host impl in repo | — | delete or real host | high |
| 8 | dead path | P3 | `modules/Search/SearchModule.cpp:32-34` | Semantic search permanently disabled (`nullptr` embedder) though compiled+tested | — | implement provider or drop | high |
| 9 | dead infra | P3 | `modules/UpdateManager.cpp:14-17` + `CMakeLists.txt:111-113,172-184` | Plugin/update/theme toolchain unwired; `ZipExtractor.cpp` not in any target and needs absent `miniz` | — | prune or wire | high |
| 10 | test/build hygiene | P3 | `CMakeLists.txt:316-362` etc. | Test targets duplicated link/Includes; gtest `.cc` compiled from absolute vcpkg paths; `gtest_add_tests` vs `gtest_discover_tests` mixed; tests drop config JSON into `rex-core/` | — | `rex-test-common` helper; `GTest::gtest_main`; temp dirs | high |

Also: `EventBus` has no `unsubscribe`; stored subscriber captures can outlive unloaded modules.

### 5.10 App (Electron + UI) + Surface + rex-shell

Verdict Electron: architecturally strong (persistent client, stale-teardown guards, init de-dup,
supervisor w/ recovery+poison-park) but the mission-critical live-update handler is dead-on-arrival
(P0), audio mode is unreachable end-to-end, and shutdown timing truncates clean exits.

| # | type | sev | file:line | title | evidence | fix direction | conf |
|---|------|-----|-----------|-------|----------|---------------|------|
| 1 | bug (IPC) | **P0** | `main.cjs:417,428` | `wallpaper:update` ReferenceError on **every** call (`audioMode` not destructured; `!= null` itself throws); mode/loop/speed/volume/mute live updates all broken; user toast per settings change | node repro of handler body → ReferenceError; introduced `8e13cd90` 2026-10-14; caller `desktop.js:102→preload:12`; toast `desktop.js:105-106` | add to destructure + validate; add handler test | high |
| 2 | bug | P1 | `main.cjs:390-398,125-133` + `DesktopApply.jsx:35-43` | Audio mode never applied on apply/reapply and never triggers an update (LOAD omits it; `settingsKey` omits `playback.audioMode`; no `SET_AUDIO_MODE` call) | — | send via LOAD or post-LOAD; include in key + reapply | high |
| 3 | bug | P2 | `desktop.js:87` + `main.cjs:423` | Live image fill-mode sends values the engine rejects (`'cover'/'stretched'` vs `fill/fit/stretch/center/tile`) → skipped + ERROR toast; initial `wallpaper:set` path maps correctly | `RendererAdapters.cpp:9-16`; `Engine.cpp:1047-1053` | map in update path too | high |
| 4 | bug (lifecycle) | P2 | `wallpaperRuntime.cjs:219-226` | 3 s kill fallback vs native ~5 s telemetry join → clean shutdown truncated (~40 % of quits hard-kill before `engine->Shutdown()`) | `wallpaper_cli_main.cpp:662,819-820,827` | slice telemetry sleep or raise fallback | high |
| 5 | bug | P2 | `main.cjs:417-430` | Live property updates never persisted to `lastApplied` → reapply after crash replays stale values | `reapplyLastWallpaper :117-143` | debounced persist on success | high |
| 6 | bug (robustness) | P2 | `wallpaperRuntime.cjs:143-151,173-189` | INIT timeout can wedge client ("already initialized" forever, no reset path) | native `:834-836` | kill/restart on timeout or idempotent INIT | high (mechanism) |
| 7 | bug (watchdog) | P2 | `main.cjs:608-611,634` + `supervisor/supervisor.cjs:127-151` | Hang detection parked (`missLimit: Infinity`) although native `PING` ships and is verified | `wallpaper_cli_main.cpp:804-810`; `verify-supervisor.cjs:40-43` | lower limit (e.g. 3) + capability probe | high |
| 8 | bug | P2 | `main.cjs:583-585` vs `:449-458` | "Launch at login = off" overridden every launch (`applyLoginItem(true)` unconditional on whenReady) | — | respect persisted pref; default-enable first run only | high |
| 9 | dead code | P3 | `supervisor/supervisor.cjs:56-59,93-95,97-103,162` | Renderer watchdog unreachable (no callers, no `render-process-gone` hookup); crashed renderer never reloaded | greps | wire or delete | high |
| 10 | bug (diagnostics) | P3 | `main.cjs:55,632` + `supervisor/crash-reporter.cjs:98` | Crash reports never include the app log (path mismatch `rex-app.log` vs `<temp>\REX\rex-app.log`) | — | align paths | high |
| 11 | packaging | P3 | `app/package.json:23-52` + `supervisor/minidump.cjs:14-21` | `rex-minidump.exe` not bundled → no dumps in packaged builds | — | add to extraResources | high |
| 12 | bug (recovery) | P3 | `wallpaperRuntime.cjs:50-59` + `supervisor.cjs:184` | Spawn failure never reaches supervisor ('exit' not emitted; tray availability never set) | — | emit failure event | high |
| 13 | bug (diagnostics) | P3 | `wallpaperRuntime.cjs:55-59,75` + `supervisor.cjs:186` | Runtime-exit crash reports always `processId: null` (pid nulled by `_teardown` before emit) | — | capture pid before teardown | high |
| 14 | IPC validation | P3 | `main.cjs:486-489` | `wallpaper:delete-thumbnail` prefix check missing `path.sep` + case-sensitive | correct guard `wallpaper-library.cjs:61-62` | compare with separator; normalize | high |
| 15 | dead surface | P3 | `preload.cjs:13-19` + `desktop.js:27-37` | Dead preload APIs (`getCapabilities`, pause/resume, login-item, platform/isDesktop); hardcoded capabilities gate never follows real adapter support | greps | delete or wire | high |
| 16 | UI dead controls | P3 | `ActionsSection.jsx:26-49`; `PlaybackSection.jsx:21-33`; `AudioSection.jsx:11-23` | No-op buttons (8× `noop`); transport buttons only mutate store; duplicate audio controls writing a field the apply path never reads | — | hide/disable; merge audio controls | high |

- Surface (`src/ui/surface/`): coherent, strictly-typed Phase-1 framework; **not imported anywhere in
  the shipping app** — library alive, product integration dead.
- rex-shell: legacy WinUI 3 shell whose backend pipe is never served in the current tree; last commit
  2026-07-13; `REX.Shell.Tray` has only `bin/obj` leftovers. Dead parallel track — excluded from the
  engine mission.
- Verified-good: single-instance guard, close-to-tray, `wallpaper:set` validation, frame-protocol
  correlation/teardown guards, oversized-frame kill→recovery, staged apply effects, clean widgets
  deletion (no dangling imports), dev/packaged runtime path resolution.
- Uncommitted user WIP (`App.jsx`, `SidebarLayout.jsx` modified; widgets/* deleted) existed during
  the audit and was left untouched; findings reflect the tree as read.

---

## 6. Dead code & hygiene inventory (report-only — no deletions)

**Orphaned subsystem:**
- `rex-core/src/wallpaper/renderer/` (15 files: AudioVisualizerRenderer, ImageRenderer, RenderPipeline,
  SceneLoader/Renderer, ShaderRenderer, ECS, IRenderer, RendererManager …) — **absent from all CMake
  targets, cannot compile as-is** (`WaitForExternalFence` doesn't exist), yet it is the only caller of
  several compositor APIs and it plants latent bugs. Live renderer is `src/graphics/renderer/`.
  **Owner call: delete or move to an explicit archive location.**

**Compiled-but-unreferenced:**
- `platform/windows/Windows.cpp/.hpp` (all stubs, zero callers), `core/scheduler/render_scheduler.*`
  (zero call sites), `shared/unique_handle.hpp` (no includers), `graphics/animation/*`,
  `graphics/effects/*` (NoiseTexture, effect_renderer), `profiles/**` (ProfileManager never
  instantiated), `modules/Sync/*`, `modules/Widgets/services/WidgetRuntime.cpp`,
  `modules/UpdateManager.cpp`, `desktop_personalization/**` (factory never called),
  `modules/ModuleManager.cpp`, Search semantic path.
- `src/profiles` vs `desktop_personalization/services/ProfileManager` — two ProfileManager classes.

**Not in any target (uncompiled):**
- `desktop_personalization/theme/ZipExtractor.cpp` (+ needs absent `miniz.h`), `theme/detail/Json.hpp`,
  `ThemeSchema.hpp`, `ThemeTypes.hpp`; `src/workerw.cpp` (legacy); `CurlMarketplaceClient.cpp`
  (default-OFF flag).
- Test files not built: `test_compositor_render.cpp` (only real D3D12 compositor smoke!), `test_workerw.cpp`
  (legacy header, cannot link), `test_startup.cpp`, `test_settings.cpp`.

**Root clutter (keep/archive proposal — nothing deleted without owner OK):**

| File(s) | Age | Proposal |
|---|---|---|
| `hdr_*.log` (11 files, Aug 29) | 7 weeks | ACTIVE investigation per AGENTS — keep, or archive to `docs/` folder on close |
| `*.obj` (`servo_check`, `probe_dwm`, `hdrprobe`, `present_repro`) | Aug-Oct | archive/delete (build artifacts) |
| `repack*.log` (4) | Aug 25-26 | archive/delete |
| `e2e_a/b.png`, `wall_a/b.png` (≈10 MB) | Aug | archive/delete (one-time comparisons) |
| `--help` (130 KB junk file) | Sep 5 | delete candidate (accidental runtime output; also one in `rex-core/`) |
| `PresentMon-2026-10-16T212704.csv` (4 MB) | Oct 16 | keep until campaign docs finalize, then archive |
| `rex-runtime.log`, `rex-cli.log` | Aug 8 | delete candidates |
| `sessions/` (1 file, 2026-07-13) | old | keep (convention) |
| `test_settings_config.json` at root | — | stray test artifact (tests write cwd) — move test writes to temp (§5.9 #10) |

**Stale docs:** `#project_status.md` "Shell UI" section (July audit, pre-Electron), `#project_status.md:223`
claims Personalization ✅ though adapters are stubs (§5.9 #4); `everything.md` root-map lists stale
artifacts; TODO.md:154 / everything.md:142 overstate A2 closure (§3.1).

---

## 7. Fix batch proposal (triage — user picks order)

1. **Batch 1 — user-visible + stutter-linked (proposed):** Q1 (P0 ReferenceError), A1 (P0 audio ring
   OOB), D1 (P1 EAGAIN retry — the intermittent start-stutter suspect), AU2 (P1 EXTENSIBLE parse),
   Q2 (audio mode end-to-end), D5/Q2 tie-in fill-mode mismatch. Then a user re-watch on Aari/OLED
   with instrumentation before/after.
2. **Batch 2 — lifetime/race P1s (engine/video/platform/graphics):** E1, P1, P2, W1, G1, G2, R1, R2, R3.
3. **Batch 3 — robustness P2s:** AU3-10, D2/D3/D5/D6, PV4/5/6/7/8, RT1-3, APP3-8, G3-9.
4. **Batch 4 — hygiene & tooling:** delete/archive orphaned `wallpaper/renderer/`, dead modules,
   unbuilt tests, root clutter; restore vcpkg; add `.clang-format`/`.clang-tidy`; real CI lint;
   `/W4` census; test-target cleanup; coverage gaps (§3.3).

Every finding above is filed at summary level in `TODO.md` 🐛 backlog with references back to this
document.
