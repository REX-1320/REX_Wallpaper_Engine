# SDD ledger — plan: docs/superpowers/plans/2026-10-07-realtime-playback-clock.md (v2 clean plan)

## Setup (2026-10-07)
- Execution: native inline (user confirmed). No worktree: Ruling — work in-place on main (session convention; all prior work units committed on main; worktree would break machine-local `.build` + GPU harness paths) — cost if wrong: none, builds are local-only.
- TDD + systematic-debugging + verification-before-completion loaded.
- Pre-flight shared-interface rows:
  - T1→T5: T1 moves hot-reload before BeginFrame; T5 rewrites the same Begin/Draw/Present block — T5 must preserve R1 ordering. OK, sequential.
  - T3→T4/T5: T3 plumbs `framesDropped`/`repeatedFrames` with TODAY's semantics; T4 upgrades drops to scheduler-honest, T5 starts counting repeats. Ruling — stage the semantics, note upgrade points in struct comments — cost if wrong: telemetry means different things across commits (mitigated: each commit documents it).
  - T5→T1/T2: T5's video-branch-only guard depends on T1 ordering + T2 static gating. OK, sequential.
  - T6→T3/T5: consumes Hz detection (T3) + cadence (T5). OK, sequential.
  - Pre-flight: rows recorded, no blocking conflicts.

## Task 1 (Phase 0 R1) — complete
- Commit: fix(engine): R1 hot-reload frame-lock self-deadlock (+9 files). Tests: FrameLockGuardTest 3/3 GREEN (RED was C1083 on missing header); full rex-wallpaper-tests has 2 pre-existing failures, both named: EngineTest.VideoScenePlayback (needs test.mp4 in CWD — documented test-gap) and StressTest.RapidSceneSwitching (expects success for Z:/missing/clip.mp4 — same gap family). Neither touches BeginFrame/LoadTexture paths. Live: CLI static load + file overwrite -> Hot-reloaded static image in cli.log, PID 1832 alive after (old code deadlocks the render thread here).
- Ruling: ClearTexture left unguarded (returns void so fail-loud needs a signature change; zero in-frame callers: MonitorCompositor:136, Engine:410,503 all outside frames) — cost if wrong: a future in-frame ClearTexture hangs instead of failing.
- Ruling: reload block placed BEFORE the dirty gate (not just before BeginFrame) + arms dirty on success — fixes R19 starvation as a side effect; static visibility now leans on Task 2 gating — cost if wrong: none, Task 2 owns the gate.
- Ruling: whole-file commit swept co-located verified-but-uncommitted hunks (P0-2 HDR init, P0-3 RGB-video shader branch, P1-5/P1-7 engine hunks, A11-A13 color fix, pending test registrations) — commit message lists them; final reviewer to confirm — cost if wrong: blame granularity.

## Task 2 (Phase 0 R2) — complete
- Commit: fix(engine): R2 static render-once (786570b). Tests: StaticRenderOnceTest RED (91 -> 180 presents in 1.5 s idle, no changes) then GREEN (frozen); full suite: same 2 pre-existing fixture failures, nothing new. Live: CLI static 0.33 s CPU / 20 s idle; touch -> Hot-reloaded + single re-present; no hang.
- Note for reviewer: static scenes no longer get per-pass audio-spectrum feeds while idling (spectrum block sits after the gate). P2-16 owns spectrum-outside-gate separately; no static+visualizer feature exists today — cost if wrong: a future audio-reactive static wallpaper would freeze its visualization (would need P2-16 first).

## Task 3 (Phase 1 telemetry) — complete
- Commit: feat(telemetry) Phase 1 (2120200). Tests: StageTelemetryTest 3/3 GREEN (RED was C2039 on missing members); full suite: same 2 pre-existing fixture failures. Live Case B on AV1 4K60: source 59.94, decode/convert/render/present ~32.6, schedulerDrops 3, queueDepth 0, repeatedFrames 0, displayHz 160, decodeMsP50 0.43ms — matches expectations (decode is the ceiling; drops appear in Phases 2-3).
- Ruling: staged-semantics as pre-flighted — schedulerDrops = legacy decoded-minus-consumed until Phase 2 PTS-honest counting; presentFps measured (not renderFps copy) so it stays honest for static scenes too; convertFps counted controller-side (DecodeFrame-ok + published) with no decoder change — cost if wrong: none, Phase 2/3 upgrade in place.
- Incident: concurrent session rewrote test_video_pipeline.cpp mid-task (appended StageTelemetry tests lost once; impl edits intact) — re-appended, rebuilt, verified tests present in binary via --gtest_list_tests. Commit message notes it. Lesson: commit promptly; test files are shared ground.

## Task 4 (Phase 2 scheduler + pacing) — complete
- Commit: feat(playback) Phase 2 (4cf30f1). ROOT CAUSE (live-proven, VCN theory refuted): Windows sleep quantization — sleep_for(16.7ms) wakes after ~31ms; pacingMs 16.67 + decodeMsP50 0.5ms yet decoderFps 33.0. Fix: PreciseSleepUntil in DecoderThread. Live GREEN: H.264 1440p60 33.0 -> 59.3fps; AV1 4K60 33 -> 59.1fps (decodeMsP50 0.3ms); both 1x, pres 1:1, drops ~0.
- Tests: FS08-FS12 + WrapTargetTest x4 + B-jitter/short-wrap (RED: C2039 / live 33fps); PreciseSleepTest (tool pin, 166ms); suite: same 2 pre-existing fixture failures. TDD note: timing RED was the live 33fps measurement (pacing_poll), unit tests pin mechanism + contract.
- Dead ends (documented in code + plan, do not reintroduce): (1) flat-out decode -> 8-slot ring holds only future frames -> starvation (219 presents/75s) + drops >1.5M; (2) naive PTS comparison across loop-phase drift (decode a loop behind wall -> all-future -> freeze/burst); (3) PTS-ahead park incompatible with ring size. Shipped instead: accurate paced decode + PTS-due selection + WrapTarget + queue park + threshold IsBackwardJump.
- Ruling: reverted my own flat-out design within the same task after live evidence killed it (systematic-debugging over plan-following; plan file updated with the lesson) — cost if wrong: none, live numbers decide.
- Ruling: IsBackwardJump threshold (>1s or lands ~0) changes P1-5 semantics for B-frame jitter (previously cleared every few frames, harmless under latest-wins, fatal under PTS selection) — existing Discontinuity tests still green; short-loop (<1s content) wrap relies on the lands-~0 arm — cost if wrong: missed clear on exotic content (fail-safe direction: stale frame shows briefly).
- Housekeeping: one pointless blank-line remove/re-add in test_frame_scheduler.cpp today (wrong-file edit, instantly reverted, net-zero — verified via reasoning, not diff).
