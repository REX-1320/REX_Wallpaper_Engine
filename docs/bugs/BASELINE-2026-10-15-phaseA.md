# Phase A re-baseline — elevated PresentMon, 10×10 runs

Date: **2026-10-15** · Box: 3840×2160 @ **160 Hz**, RX 6700 XT · Build: `fc0695d` (tree clean)
Safety: SDR-only clips, graceful `SHUTDOWN` with 5 s force-kill fallback, zero monitor blanks.

## Why this re-run exists

`TODO.md` carried a **caution** recorded from an earlier Phase A:

> at 160 Hz, obanai-class playback is already well-locked (`AE_p50` -0.04..+0.02,
> `AE_p95` 0.53-1.71 over 10 runs) — the phase-lock defect previously measured was
> observed at **120 Hz**. A servo must not be built for a defect that does not
> reproduce at the user's current refresh rate; re-measure before implementing.

This re-run **does not reproduce that caution**. obanai `AE_p95` over 10 identical
runs is **bimodal** (0.43/0.52 vs 4.42–5.46), not tight. The phase-lock defect is
**live at 160 Hz**, so Phase B2 is justified. See "Consequences" below.

## Method

* `rex-wallpaper-runtime.exe --runtime`, length-prefixed JSON protocol
  (`INIT` → `LOAD` → `PLAY` → 3 s settle → capture → `SHUTDOWN`).
* `INIT` with `maxFps=60, pauseOnFullscreen=false`; audio muted.
* PresentMon **2.6.0**, **elevated** (`Start-Process -Verb RunAs`), `--timed 30`,
  `--process_name rex-wallpaper-runtime.exe`.
* 10 runs per clip, 30 s each.

### Column mapping (this is the part that is easy to get wrong)

PresentMon 2.x renamed most 1.x columns. Mapping per PresentMon issue #468,
corrected and confirmed by Intel (markgalvan-intel, 2025-06-30):

| 1.x name (milestone's trusted list) | 2.x column used here |
|---|---|
| `FrameTime` | **`MsBetweenAppStart`** |
| `GPUTime` | `MsGPUTime` |
| `CPUBusy` | `MsCPUBusy` |
| `DisplayLatency` | `MsUntilDisplayed` |
| `AnimationError` | `MsAnimationError` |

⚠️ **`FrameTime` is `MsBetweenAppStart`, NOT `MsBetweenDisplayChange`.** The
"FrameTime → MsBetweenDisplayChange" mapping floating around is **wrong** and was
explicitly retracted: `MsBetweenAppStart` is the frame-start-to-frame-start delta
on the pipeline **input** side; `MsBetweenDisplayChange` is the **output** side.
`--v1_metrics` is **not** a fix — in 2.6 it emits a *partial* legacy set with no
`FrameTime`, `AnimationError` or `CPUBusy` at all.

`DisplayedTime` / derived display deltas: **never cited** (non-monotonic, retired).

## Results — 10 runs each, median of run medians

### obanai — 4K H.264 60 fps @ 160 Hz

| Metric | Median | Range over 10 runs |
|---|---|---|
| `FrameTime` p50 (ms) | **16.998** | 16.996 – 17.013 |
| `FrameTime` p95 (ms) | 17.228 | 17.170 – 17.278 |
| `GPUTime` p50 (ms) | 12.750 | 12.726 – 16.615 |
| `GPUTime` p95 (ms) | **17.112** | 17.001 – 17.203 |
| `GPUTime` max (ms) | 29.294 | 29.227 – 38.460 |
| `CPUBusy` p50 (ms) | 16.936 | 16.924 – 16.947 |
| `AnimationError` p50 (ms) | −0.008 | −0.023 – +0.003 |
| `AnimationError` p95 (ms) | **4.548** | **0.425 – 5.462 (BIMODAL)** |
| frames / 30 s | 1764 | 1761 – 1767 |

`AnimationError` p95 per run: `0.43, 0.52, 4.42, 4.42, 4.55, 4.61, 4.71, 4.72, 4.77, 5.46`

`PresentMode` = **`Hardware Composed: Independent Flip`** in **10/10** runs.

### Aari — 4K AV1 24 fps @ 160 Hz

| Metric | Median | Range over 10 runs |
|---|---|---|
| `FrameTime` p50 (ms) | **41.970** | 41.924 – 42.018 |
| `FrameTime` p95 (ms) | 42.376 | 42.208 – 42.564 |
| `GPUTime` p50 (ms) | 22.912 | 22.892 – 39.178 |
| `GPUTime` p95 (ms) | **85.318** | 85.188 – 85.380 |
| `GPUTime` max (ms) | 109.398 | 109.315 – 113.776 |
| `CPUBusy` p50 (ms) | 41.892 | 41.848 – 41.950 |
| `AnimationError` p50 (ms) | 2.636 | 0.447 – 7.706 |
| `AnimationError` p95 (ms) | **34.143** | 30.318 – 35.748 |
| `BetwDisplay` p50 (ms) | 38.401 | 34.396 – 41.611 |
| `BetwPresents` p50 (ms) | 41.977 | 41.928 – 42.026 |
| frames / 30 s | 713 | 709 – 714 |

`PresentMode` = **`Hardware Composed: Independent Flip`** in **10/10** runs.

---

## Finding 1 — a systematic +0.30…0.33 ms per-frame pacing bias (NEW)

Both clips present **slower than nominal**, by a near-identical absolute per-frame
amount:

| clip | nominal | measured `FrameTime` p50 | bias/frame | achieved fps | deficit |
|---|---|---|---|---|---|
| obanai 60 fps | 16.6667 ms | 16.998 ms | **+0.3313 ms** | 58.80 / 60 | 2.00 % |
| Aari 24 fps | 41.6667 ms | 41.970 ms | **+0.3033 ms** | 23.77 / 24 | 0.97 % |

A **constant** per-frame overshoot predicts the rate deficit exactly:

* obanai: `0.3313 ms × 58.80 fps = 19.48 ms/s` drift → predicted `58.83 fps`; **measured 58.80**
* Aari: `0.3033 ms × 23.77 fps = 7.21 ms/s` drift → predicted `23.83 fps`; **measured 23.77**

Agreement within 0.05 % (obanai) and 0.25 % (Aari). Two different frame rates, two
different budgets, one constant additive term — that is the signature of a
**per-sleep wake-up overshoot**, not a cadence or rounding error.

Prime suspect: `PreciseSleepUntil` (`shared/PreciseSleep.hpp:28`) uses
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` for sleeps > 2 ms (`:44-48`) and its own
header documents accuracy as "**~0.5 ms**". A ~0.3 ms mean overshoot is squarely
inside that. **Not yet isolated to a single call** — needs a dedicated A/B (spin-only
vs waitable-timer), do not treat the suspect as proven.

**This is directly in Phase B2's scope:** it is a closed-loop-correctable error, it
is measurable, and it is the same class of defect as a phase offset.

## Finding 2 — obanai `AE_p95` is BIMODAL at 160 Hz (refutes the recorded caution)

Two clean clusters over 10 identical runs: **~0.47 ms** (2 runs) and **~4.4–5.5 ms**
(8 runs). Nothing in between. That is the signature of **two distinct startup
phase-lock basins** — the exact mechanism described in the 2026-10-14 campaign
("mid-window lock → clean run; lock ~1 ms before deadline → no margin → slips every
frame"). The recorded caution claimed a tight `0.53–1.71`; that is **not** what this
box produces.

⇒ **Phase B2 is justified.** The defect reproduces at the user's current 160 Hz.

## Finding 3 — Aari is GPU-bound by 2×; obanai is GPU-marginal at p95

* **Aari:** `GPUTime` p95 = **85.3 ms against a 42.0 ms budget (203 %)**. Confirms the
  driver AV1 decode-hog mechanism (`TODO.md` mechanism 1) and bounds Phase D's win:
  the submit side is already clean (Phase C GO), so the ceiling is the residual
  `AE_p95` 30–36 ms.
* **obanai:** `GPUTime` p95 = **17.11 ms against a 17.00 ms budget (101 %)**. Marginal —
  p95 GPU work already exceeds the whole frame budget. Any servo must not add GPU work.

Note Aari's `BetwDisplay` p50 (38.4 ms) ≠ `BetwPresents` p50 (42.0 ms): presents are
**not** landing 1:1 on display changes, i.e. some coalesce. For obanai the two match
(16.99 vs 17.00).

## Finding 4 — a WorkerW wallpaper **does** independent-flip on this box

`PresentMode` = `Hardware Composed: Independent Flip` in **20/20 runs** across both
clips.

This **contradicts** the recorded inference in `_research_present_pacing.md:493-497`
and `wall_research.md` that a WorkerW-hosted wallpaper is *"always Composed: Flip …
can never use the independent-flip path"*. On this hardware/driver the wallpaper
surface is promoted to a hardware overlay plane and **bypasses the DWM compositor
cadence entirely**.

Consequences — this changes the model the servo is built on:

1. The premise "the wallpaper is permanently a composed present, therefore
   permanently subject to the DWM compositor cadence" is **false here**.
2. A `DwmGetCompositionTimingInfo`-based phase reference would be referencing the
   **DWM** cadence, which this surface is **not** locked to. **Re-verify the phase
   reference choice before implementing B2** — an independent-flip surface syncs to
   the display, not to DWM.
3. It also weakens the "a wallpaper cannot benefit from FreeSync" argument.

Per the milestone's "measure, do not assume" rule, this needs one confirming run
(e.g. `PresentMon` with `--write_display_metadata`, or an A/B with `OverlayMinFPS`).

## Consequences for Phase B2

* **Proceed** — the phase-lock defect reproduces at 160 Hz (Finding 2).
* **Re-verify the phase-reference API first** (Finding 4): `DwmGetCompositionTimingInfo`
  is the right candidate *if* the surface is composed; on an independent-flip surface
  the correct reference is the **display** vblank (`IDXGIOutput::WaitForVBlank` or the
  DXGI presentation scheduler `SyncRefreshCount`/`SyncQPCTime`). The already-fixed
  `SampleDisplayCadence` (`SwapChain.cpp:169`) feeds exactly that and works.
* The servo has **two** correctable error terms now: phase offset **and** the
  +0.3 ms per-frame rate bias (Finding 1).

## Finding 5 — `DwmGetCompositionTimingInfo` is the WRONG clock here (blocker resolved)

Finding 4 raised a blocker for Phase B2: if the surface independent-flips, the
DWM composition cadence may not be the clock REX is actually locked to. **Resolved
by direct measurement** with a standalone probe (no engine change) that samples
`DwmGetCompositionTimingInfo(nullptr, …)` and converts the `qpc*` fields
(`QPC_TIME = ULONGLONG`, `dwmapi.h:160`) through `QueryPerformanceFrequency`.

**Panel (ground truth):** 3840×2160 @ **160 Hz** ⇒ 6.2500 ms.

| condition | `qpcRefreshPeriod` | implied rate | `qpcVBlank` vs `qpcCompose` |
|---|---|---|---|
| idle | 6.2495–6.2511 ms **and** 7.1820–7.2158 ms (oscillates) | 160 Hz ↔ 138.6 Hz | identical |
| **REX playing obanai** | **7.2158 ms, constant, 24/24 samples** | **138.585 Hz** | **identical, 24/24** |

`rateRefresh = 10000000/72158` confirms the 7.2158 ms figure.

**Conclusion:** while REX runs, DWM reports a **138.585 Hz** composition cadence
against a **160 Hz** panel — a **15.45 % period error** (`7.2158 / 6.25 = 1.1545`),
and `qpcVBlank == qpcCompose` exactly every sample, i.e. **DWM runs no distinct
composition pass for this surface** — exactly what Finding 4's Independent-Flip
result predicts.

A phase servo locked to this clock would drift by 15.45 % of elapsed time
(~15.5 ms per second). **`DwmGetCompositionTimingInfo` must NOT be used.**

⇒ **Phase B2 must use the DXGI presentation scheduler instead**: per-swapchain
`SyncRefreshCount` + `SyncQPCTime` from `IDXGISwapChain::GetFrameStatistics`, which
is (a) already implemented and now working after `9437032` (verified `dwell=1/2/3/3`),
(b) references *exactly* the scheduler that quantises our `Present(1,0)`,
(c) is HDR-independent, non-blocking and safe every frame, and
(d) gives both the period (`ΔQPC / Δrefresh`) and the phase (`SyncQPCTime` mod period)
from one call.

**This overturns the `DwmGetCompositionTimingInfo` recommendation** recorded in
`TODO.md` before this measurement. The API research was correct about the *contract*
(hwnd must be NULL, non-blocking, HDR-independent); it was wrong about which clock
governs this surface on this hardware.


Harness: `%LOCALAPPDATA%\Temp\opencode\rex-baseline.ps1` (not committed — Phase A is
"no code change"). Raw CSVs + per-run telemetry deltas under
`%LOCALAPPDATA%\Temp\opencode\baseline\{obanai,aari}\`.
DWM clock probe: `%LOCALAPPDATA%\Temp\opencode\probe_dwm.cpp` (also not committed).
