# REX Real-Time Playback Clock v2 — Clean Plan (decode × timeline × presentation separated)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decoder throughput must never control playback time. Three separated concepts — source timeline (PTS clock), decode throughput (measurement), presentation cadence (display-driven) — so AV1 4K60 at a 33 fps VCN ceiling plays at 1.0x with ~33 unique frames/s and counted drops.

**Architecture:** `CadenceController` computes a maximum-useful `presentationTarget = min(sourceFps × rate, displayHz, userCap)` from `MediaInfo` + display topology; the decode thread publishes flat-out into the existing 8-slot GPU pool; the PTS scheduler answers "which decoded frame is due at wall time T"; each compositor presents at its own display cadence, repeating the last frame when nothing new is due. Conversion stays immediate GPU-side (no lazy-conversion refactor in this plan).

**Tech Stack:** C++20, FFmpeg (libavcodec D3D11VA/SW), D3D11→D3D12 shared fences, DXGI display topology, gtest.

**Spec:** User clean plan 2026-10-07 §§1–20. Non-negotiables: never force decoder FPS to monitor Hz (§1, §20); `sourceFps` / `decodedFps` / `presentFps` as distinct values (§2); PTS-authoritative scheduling, no `sleep(16ms)` / frame-counter scheduling (§5); HW path stays zero-copy D3D11VA→D3D12, no GPU→CPU→GPU for normal video (§6); latest-frame-wins, drop stale, never queue latency (§§8–9); render on display cadence via VSync/frame-latency wait, no 250 Hz spin (§§10–11); static renders once (§12, fixes open R2); dynamic Hz changes without restart (§13); one decoder, shared timeline, per-monitor cadence at `max()` demand (§14); speed scales the video timeline (§15, audio-tempo stays P1-13 follow-up); per-stage diagnostics before optimization (§§17–18 — we already have Case B: decode≈convert≈render≈present≈33); do not build on open critical bugs R1/R2 (§19 — Phase 0 fixes exactly those two, both in the touched code).

## Global Constraints

- SPSC invariant: one producer, one consumer, no hot-path mutexes (acquire/release atomics only). Ring stays 8 slots (hard max); decode-ahead target 2–3 frames.
- `Tick` stays non-blocking; returned pointer valid until next `Tick`; consumer waits on slot fence before sampling (unchanged).
- Backward-PTS jumps clear via `IsBackwardJump` (strict `<`; equal-PTS repeats are not seeks) — P1-5 preserved.
- `m_speed` clamp 0.25–4.0 unchanged; speed scales the wall target only.
- `maxFps`/userCap honored as an upper bound inside `presentationTarget`; never as a decoder throttle.
- Audio path untouched (mute/volume/tempo = P1-13 follow-up, filed not fixed).
- Build: offline passthrough toolchain + single-`.vcxproj`; run from `.build\Release\`.

## Review Focus

- Loop wrap: backward jump clears selection state AND drop baselines — no phantom drops spike — pinned in Phase 2.
- Seek while decode is ahead: future frames never present; re-arm clean — pinned in Phase 2.
- Pause: wall clock stops, decode-ahead cap parks the decoder (no whole-file buffering) — pinned in Phase 2.
- `speed = 2.0` on 60 fps × 60 Hz: timeline demand 120, presentation capped 60, drops rise, PTS exact — pinned in Phase 4.
- Hz change 160→60 mid-playback: cadence recalculates, timeline continues, no restart — pinned in Phase 4.

---

### Phase 0, Task 1: Fix R1 hot-reload deadlock (pre-req, same code region)

Why first: Phase 3 rewrites the Begin/Draw/Present block; building on a known CRITICAL deadlock there is forbidden by the spec §19.

**Files:**
- Modify: `rex-core/src/wallpaper/engine/Engine.cpp` (render loop ~1414–1477: move hot-reload block ~1448–1464 to before `BeginFrame`)
- Test: `rex-core/tests/wallpaper/test_engine.cpp` (or new `test_hot_reload.cpp` if fixtures don't fit)

**Interfaces:**
- Consumes: `renderHotReloadPath/renderHotReloadLastWrite` snapshot (exists).
- Produces: identical hot-reload behavior, executed before `BeginFrame()`; never calls `LoadTexture` between `BeginFrame` and `EndFrame`.

- [ ] **Step 1: Write failing test** — render-thread hot-reload path change completes while a frame is in flight; assert no same-thread re-entrant `Begin/End` (model with the existing engine fixture; deadlock itself is proven by code inspection + live run, the test pins ordering).
- [ ] **Step 2: Run** `ctest -R wallpaper --output-on-failure` — Expected: FAIL (reload runs inside Begin/End).
- [ ] **Step 3: Implement** — move the block from `Engine.cpp:1448-1464` to before `m_renderer->BeginFrame()` (~1417); keep timestamp publish-back under `m_mutex`. R19 starvation note: the moved block runs every pass for video scenes (not gated on dirty), static scenes keep gate behavior per Phase 0 Task 2.
- [ ] **Step 4: Verify** — full wallpaper suite green + live static-image hot-reload (touch file, new image appears ≤2 s, no hang over 5 swaps).
- [ ] **Step 5: Commit** `fix(engine): move hot-reload before BeginFrame (R1)` and check off R1 in TODO.

### Phase 0, Task 2: Fix R2 static continuous present (pre-req, spec §12)

**Files:**
- Modify: `rex-core/src/wallpaper/engine/Engine.cpp` (video branch ~1401–1433 + dirty handling)

**Interfaces:**
- Consumes: `m_frameDirty` (exists).
- Produces: any presented frame clears `m_frameDirty` regardless of scene type; static scene presents only when dirty (load, transform, hot-reload, apply); video branch behavior unchanged in this task.

- [ ] **Step 1: Write failing test** — static scene, two loop passes, no changes: assert exactly one present. (Fixture-gated; live run is the real gate.)
- [ ] **Step 2: Run** — Expected: FAIL (presents every pass).
- [ ] **Step 3: Implement** — clear dirty after any presented frame; arm from load/transform/hot-reload paths. Do not touch the video branch.
- [ ] **Step 4: Verify** — live static image: GPU-3D ~0% after first present; touch file → one re-present. Check off R2/P2-14 in TODO.
- [ ] **Step 5: Commit** `fix(engine): static render-once, clear dirty on any present (R2)`

### Phase 1, Task 3: Per-stage diagnostics + display Hz (§§17–18, §2)

What lands: `sourceFps` (stream), `decodedFps` (decoder thread rate), `convertFps` (ConvertAndSignal rate — same thread today, kept as separate counter for the future lazy-conversion split), `schedulerDrops` (selection-skipped), `renderFps` (Draw calls), `presentFps` (presents), `repeatedFrames`, `queueDepth` (unconsumed writes), `decodeMs` (existing P50/P95/P99 ring), `displayHz` per monitor; all plumbed to `GET_STATUS` JSON.

**Files:**
- Modify: `rex-core/src/wallpaper/playback/VideoPlaybackController.{hpp,cpp}` (counters + `VideoPlaybackTelemetry` fields)
- Modify: `rex-core/src/wallpaper/video/VideoManager.{hpp,cpp}` (per-monitor `displayHz` via DXGI output `GetDisplayModeList`/current mode; refresh on topology change)
- Modify: `rex-core/src/wallpaper/engine/Engine.cpp` (~918: copy into `EngineTelemetry`; declare fields in its struct)
- Modify: `rex-core/src/wallpaper_cli_main.cpp` (~1129–1159: JSON keys)
- Test: extend `rex-core/tests/wallpaper/test_video_pipeline.cpp` (field presence + default values)

- [ ] **Step 1: Write failing test** — telemetry struct carries the new keys; `displayHz > 0` on this machine's primary monitor.
- [ ] **Step 2: Run** — Expected: FAIL (keys/Hz missing).
- [ ] **Step 3: Implement** — counters at each stage boundary (decode/convert/scheduler/render/present), Hz detection with topology-refresh hook, JSON plumbing. `framesDropped` semantics change documented in the struct comment (stale-or-skipped from here on).
- [ ] **Step 4: Verify live (the §18 question, AV1 4K60)** — Expected Case B printout: source 59.94, decode≈convert≈render≈present≈33, schedulerDrops≈0 (nothing to skip when decode is the ceiling — drops appear in Phase 2+3 when presentation outruns decode), `displayHz` = real monitor rate (160 per WMI — confirm via DXGI).
- [ ] **Step 5: Commit** `feat(telemetry): per-stage fps, drops, displayHz`

### Phase 2, Task 4: PTS scheduler + accurate paced decode (clock separation, §§5,8–9)

> PROVEN LIVE 2026-10-07: the ~33 fps lock on all 60 fps content was
> Windows sleep quantization (sleep_for(16.7 ms) wakes after ~31 ms),
> NOT decoder throughput. Shipped: source-rate pacing via
> shared::PreciseSleepUntil + PTS-due selection + loop-relative target +
> queue-depth park + B-frame-tolerant IsBackwardJump. Do NOT reintroduce:
> flat-out decode (starves an 8-slot ring: 219 presents/75 s, drops
> > 1.5M) or naive PTS comparison across loop-phase drift. AV1 4K60
> refutes any VCN-ceiling theory: 59.1 fps, decodeMsP50 0.3 ms.

**Files:**
- Modify: `rex-core/src/wallpaper/playback/FrameScheduler.{hpp,cpp}` (`Tick(targetPts)`: newest valid frame with `pts <= targetPts + 8 ms`; skipped + overwritten-unseen counted; `GetDroppedCount()`; `Clear` re-baselines)
- Modify: `rex-core/src/wallpaper/playback/VideoPlaybackController.cpp` (`DecoderThread`: delete source-rate sleep `:317–325`; sleep 5 ms only when newest published PTS > wall + 0.5 s; 10 ms when paused)
- Test: `rex-core/tests/wallpaper/test_frame_scheduler.cpp` (FS08 select-newest-due, FS09 future-not-due, FS10 skips-counted, FS11 equal-PTS-repeat-free)

**Interfaces:**
- Consumes: wall `targetPts` already passed into `Tick` via `Engine.cpp:1404` (`GetPosition`, wall-based — no change).
- Produces: decode publishes ASAP (2–3 ahead target, 8 hard max via ring overwrite); render selects by PTS; drops honest.

- [ ] **Step 1: Write FS08–FS11** (exact assertions per test names above; same style as FS01–FS07).
- [ ] **Step 2: Run** — Expected: FAIL (`Tick` ignores PTS, no counter).
- [ ] **Step 3: Implement** — ordered scan of `(consumed, writes]`, cap `kNumSlots`, select newest due, `m_lastConsumedWrite` → newest published, counter += skipped + overshoot; ahead-cap replacing the interval sleep.
- [ ] **Step 4: Verify live** — H.264 1440p60 60 s: decode rate ≥ source, drops ≈ 0. Existing FS01–FS07 + `DiscontinuityRuleTest` green (`targetPts=+inf` ≡ today's latest-wins).
- [ ] **Step 5: Commit** `feat(playback): PTS selection, flat-out decode, honest drops`

### Phase 3, Task 5: Display-cadence presentation (§§10–11) + CadenceController (§4)

**Files:**
- Create: `rex-core/src/wallpaper/playback/CadenceController.hpp` (header-only: `PresentationTarget(sourceFps, rate, displayHz, userCap) = min(...)` as max-useful cadence; `PassThrough` when source ≤ display; pure function, fully unit-tested)
- Modify: `rex-core/src/wallpaper/engine/Engine.cpp` (`PaceLoop`: video-branch interval from `presentationTarget`, floored at vsync granularity, 4 ms absolute floor kept; wait on swapchain frame-latency waitable object when available instead of `sleep_for` spin — fall back to sleep when not)
- Modify: `rex-core/src/wallpaper/engine/Engine.cpp` (video branch: re-present cached frame when `Tick → nullptr`; count `repeatedFrames`, uniques via `frameIndex`)
- Test: new `rex-core/tests/wallpaper/test_cadence_controller.cpp` (table: 60/60→60, 120/60→60, 60/160→60, 120/144→120, 30/160→30, userCap 30 clamps, rate 2.0 doubles demand)

- [ ] **Step 1: Write cadence table tests + re-present engine test** (video scene, stalled decode: N passes → N presents, uniques = 1, `repeatedFrames` = N−1).
- [ ] **Step 2: Run** — Expected: FAIL (no controller; loop `continue`s without presenting).
- [ ] **Step 3: Implement** — pure controller + video-branch-only re-present (static keeps Phase 0 Task 2 gating) + latency-object wait with sleep fallback.
- [ ] **Step 4: Verify live (acceptance)** — AV1 4K60 120 s: `presentFps` ≈ cadence (60), uniques ≈ 33, drops rising, `position` tracks wall 0.98–1.02. H.264 1440p60 120 s: all ≈ 60, drops ≈ 0. CPU at idle-video ≤ today (no spin: render thread mostly in latency wait).
- [ ] **Step 5: Commit** `feat(playback): CadenceController + display-cadence presentation`

### Phase 4, Task 6: Dynamic Hz + shared multi-monitor timeline (§§13–14) + speed (§15 video side)

**Files:**
- Modify: `rex-core/src/wallpaper/video/VideoManager.cpp` (topology/Hz-change hook → recompute targets, no restart; shared decode at `max()` monitor demand; per-compositor PTS sampling at own cadence — `PresentToAllMonitors` + per-monitor `GetPosition` `:587` already exist, wire them to the shared wall origin)
- Modify: `rex-core/src/wallpaper/engine/Engine.cpp` (`:436` speed path: assert timeline target advances at `rate`; document audio-tempo as P1-13 follow-up, untouched)

**Interfaces:**
- Consumes: Phase 1–3 primitives.
- Produces: Hz change mid-playback recalculates `presentationTarget` within one topology poll, timeline unbroken; 60 Hz + 144 Hz same-wallpaper uses one decoder, each compositor samples the shared PTS line.

- [ ] **Step 1: Write tests** — Hz-change recompute (60→144→60 sequence, no state reset); shared-timeline sampling (two cadences, one PTS origin, both 1.0x); `speed=2.0` demand doubles, PTS exact.
- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Implement** — hook into existing `CheckDisplayChanges`/topology path; shared wall origin for monitor controllers.
- [ ] **Step 4: Verify live** — change display Hz mid-playback (if the box allows; else simulate via userCap changes 60↔30): cadence follows, position unbroken. Single-monitor acceptance repeats Phase 3 gates.
- [ ] **Step 5: Commit** `feat(playback): dynamic cadence, shared multi-monitor timeline`

### Explicitly deferred (not this plan)

- Lazy conversion §7 (selection-before-convert): conversion stays immediate GPU-side; revisit only with profiling proof it matters. Counters from Phase 1 keep the split measurable.
- Audio tempo follow §15 (P1-13 owns `SetSpeed`-to-audio).
- Transcoding (rejected by spec).
- Single-runtime WorkerW guard (backlog G5; separate work unit).

---

## Self-Review

1. **Spec coverage:** §1 no-forced-decoder-rate (controller is max-useful only) ✓; §2 three FPS values = `sourceFps`/`decodedFps`/`presentFps` + `renderFps`/`convertFps` spares ✓; §§3–4 controller ✓; §5 PTS-authoritative ✓; §6 zero-copy HW untouched + SW pool already fixed by P0-3 ✓; §7 deferred with reason ✓; §§8–9 bounded latest-wins ✓; §§10–11 latency-wait cadence ✓; §12 = Phase 0 Task 2 ✓; §13–14 = Phase 4 ✓; §15 video side ✓, audio deferred ✓; §§17–18 = Phase 1 first ✓; §19 = Phase 0 (R1/R2 only — hot-reload deadlock + static present; allocator fencing already shipped P0-4, pool P0-3; the remaining P0/P1/P2 items are separate backlog, not built-upon here) ✓; §20 philosophy ✓.
2. **Step scan:** test steps carry exact assertions/values; code steps name exact files/sites/signatures; verifications carry commands + numeric gates. No transcribed bodies.
3. **Type consistency:** `Tick(double)->const VideoFrame*` unchanged; `GetDroppedCount()->uint64_t`; telemetry `double`s; `PresentationTarget(double,double,double,double)->double`. Uniform across phases.
4. **Review Focus:** five lines, each pinned to its owning phase.
5. **Proportion:** plan shorter than the diff; code appears only as signatures, sites, and gates.
