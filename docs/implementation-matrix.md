# REX — Implementation Status Matrix

> The design contract is `docs/wallpaper-engine-architecture.md` (LOCKED).
> This file is the implementation truth table: what is actually wired, tested,
> and proven right now. Four states: `verified working` · `implemented but
> unverified` · `confirmed defect` · `deferred`.
> Last updated: **2026-10-10** (P0.4 bisected; P1+P2 code-complete).

## Native engine

| Area | State | Evidence |
|---|---|---|
| Video decode HW (D3D11VA, shared NT, 8-slot pool) | verified working | 5-video fps logs + sysmon, `fps.md`; 185-test suite green |
| PTS scheduler (latest-wins, wrap handling, `loopWraps`) | verified working | live wrap-burst pinning (astral s21/s42); `FS`/`WrapTarget` tests |
| Cadence presentation target | verified working | 120fps-on-160Hz live (~116); `CadenceControllerTest` 8/8 |
| Static render-once + OS image path | verified working | `StaticRenderOnceTest`; `SetImageWallpaper` live |
| WorkerW hide/show transitions | verified working | P0.1 shipped 2026-10-10: `IDesktopHost::SetVisible` + 3 hosts + hide-on-unload/show-on-load; live matrix visible=True→False→True→True(STOP); host tests 13/13; suites green (1 foreign audio crasher untouched) |
| Stop/Play/Seek/unload timeline | verified working | `StopTimelineTest`, `PlaybackSettingsTest`; live STOP→PLAY |
| Volume/mute incl. recovery restore | verified working | `ApplyPlaybackSettings`; live `SET_MUTE` probe |
| Audio tempo + endpoint recovery | implemented but unverified | code present (P1-13, P3-G4); `MonitorAudioTest` crashing on f8d895f MMCSS change — owned by parallel track, NOT this loop |
| Protocol version + 2nd-runtime guard | verified working | live refusal probe (exit 3); handshake tests |
| Power/session suspend sources | verified working | lock/display-off/battery-saver wired to suspend check; needs unified policy (P1) |
| Unified PlaybackPolicy arbitration | verified working | P1 shipped 2026-10-10: behavior-preserving extraction (`PlaybackPolicy.hpp` + 10-test truth table); render loop calls it, bodies unchanged; suite 194 green |
| Display topology identity | verified working | P1 extended 2026-10-10: predicate now compares EDID hash + adapter LUID (populated per enumeration via existing DXGI helper); same-name physical swaps + GPU moves reconcile; 3 new tests, suite 197 green |
| Resource-limit envelope | implemented, live refusal untested | P1 shipped 2026-10-10: pure `ResourcePolicy.hpp` (footprint = w·h·4·16 slots, sanity cap 16384, fail-open on unknown budget) + LoadScene gate mirroring failed-open cleanup (ok:false OutOfMemory); slot mirror pinned vs decoder+scheduler; 6 tests, suite 203 green |
| Per-monitor present counters | implemented, live increments pending link | P2 shipped 2026-10-10: compositor `m_frameCount` plumbed → VideoManager → EngineTelemetry → GET_STATUS `monitorPresents`; aggregation test headless-green; suite 204 green |
| Native log rotation | implemented, soak-observation pending | P2 shipped 2026-10-10: spdlog rotating sink (2 MB × 2, fresh file per start preserved); no test (singleton + file side effects — noted); needs long-run size check |
| SW AV1-HDR decode | confirmed defect | `send_packet -40` (pre-existing backlog) |
| blk4k CBS-AV1 assert (ffmpeg 8.1.2) | bisected → parallel audio track (not fixed here) | audio-STREAM-dependent: 5/5 crash w/ audio, remux w/ audio crashes, audio-free plays 12s clean; Engine hunks exonerated by stash test; prime suspect f8d895f MMCSS/Start-Stop (+WER heap loop + audio-test fastfail); sibling Dolby entry in TODO |

## Electron app

| Area | State | Evidence |
|---|---|---|
| Apply image / video / clear-STOP; pause/resume; login-item; thumbnails IPC; window controls | verified working | 12 `ipcMain` handlers + preload bridge; live probes |
| Supervisor (watchdog/crash-report/recovery) | implemented + headless-verified (live pending) | P0.2 wired 2026-10-10: single exit driver (inline recovery removed), renderer guard, parked heartbeat, resetRecovery, will-quit stop; 12/12 stub scenarios green against real classes; native PING added (unlinked); live crash test blocked on binary link |
| App-state persistence (versioned + backups) | implemented + headless-verified (live pending) | P0.3 wired 2026-10-10: instantiation + get/set IPC + preload bridge (existing `useAppState` hook works unchanged); 20/20 harness incl. corrupt recovery + restart |
| Wallpaper-library persistence | implemented + headless-verified (live pending) | P0.3 wired: self-registered `gallery.*` + `read-file` bridge + renderer import/restore/remove + dedup reconciliation (library authoritative by path); missing files skipped-never-pruned |
| V8 memory cap / GPU preference gating | deferred | zero matches in tree; review asks, not scheduled |
| Error-to-toast path | implemented (live display pending app run) | P1 wired 2026-10-10: `notify.js` + root `<Toaster>` + apply/update failure toasts + ERROR/recovery-gave-up toasts; `vite build` green, lint clean |

## Validation backlog

| Item | State |
|---|---|
| 120fps ≥117.6 unique/s steady | diagnosed, below bar (116.6 measured) | split on canonical binary: decode 118.0, consume 117.8, presented 116.6; Video 58.8%/3D 22%/CPU 7% — distributed loss, no saturation; platform ceiling on RX 6700 XT, needs owner call |
| HDR-enabled (OS HDR on) validation | open (needs user action) |
| NVIDIA cross-hardware | open |
| 2h soak on current binary | open |
| black4K vs mindblow controlled rerun | superseded (mindblow steady truth already measured: 59.33 consume/s) |
