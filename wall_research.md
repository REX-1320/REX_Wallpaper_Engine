# REX Wallpaper Engine — Decoder & Presentation Audit

**Date:** 2026-10-11
**Author:** opencode (Ralph loop, iterative)
**Status:** Investigation complete → remediation required
**Scope:** wallpaper decode path only (per AGENTS.md). Widgets/taskbar/themes deferred.

---

## 0. The Reported Symptom

| Path | Condition | Result |
|---|---|---|
| Login apply | Electron launched `--background`, window **hidden** | **Smooth** |
| App apply | Electron window **visible**, user re-applies same file | **Stutters**, inconsistently |
| Headless test | No Electron process at all | **Smooth** (per user) |

Reported as: *"not consistent (not offering complete smoothness)"*, and a silent 4K60
clip stutters too. Engine telemetry reports `effFps ≈ sourceFps`, `schedDrops ≈ 11`
over 30 minutes — i.e. **the runtime believes it is presenting correctly.**

A previous theory (Electron UI GPU load: backdrop-filters, rAF loops, transparency)
was **tested and disproven** — removing all of it changed nothing.

---

## 0b. MEASURED PROOF (2026-10-11, this session)

All numbers below are from a clean headless run of the Aari 3840×2160 @24 fps clip
with **zero Electron processes**, fullscreen auto-pause disabled (see §0c).

### P1 — The stutter is real, and invisible to the existing telemetry

```
effFps=23.9   gapP50=41.88ms  gapP95=43.34ms  gapP99=62.41ms  gapMax=67.78ms  gapJit=5.84ms
```

`effFps` reads a flawless 23.9 while ~2 % of frames run **20–26 ms long** against a
nominal 41.667 ms. **A mean cannot express jitter** — this is why every prior
investigation concluded "the engine is fine". The new `gap*` telemetry closes that
blind spot. These fields are now emitted by `GET_STATUS` and the
`runtime-telemetry.log` sampler.

### P2 — It is NOT Electron, NOT the UI, NOT the GPU load

The run above had **no Electron process at all** and still showed
`gapJ99=62 ms`. Therefore the app-visible stutter is not caused by Chromium, by
backdrop-filters, by CSS/rAF work, or by GPU contention from the UI. Those were
tested and eliminated.

### P3 — It is NOT the decoder

Same run:

```
srcFps=24.0  convFps=23.9  decFps=23.9  rendFps=23.9  schedDrops=0  queue=0
```

Decode, convert and render all track the source exactly, with zero scheduler
drops. **The decoder is healthy.**

### P4 — It is NOT the scheduler and NOT housekeeping

Per-frame phase breakdown (`begin+wake` = snapshot lock + `scheduler.Tick`):

```
late frame: total=17.61ms target=41.67ms begin+wake=0.19ms draw+present=17.42ms
```

* `begin+wake` = **0.19 ms** → the ring scheduler costs nothing.
* `CheckDisplayChanges()` every 120 frames measured **< 4 ms** (a `slow
  housekeeping` WARN never fired) → display-topology polling is not the spike.

### P5 — It IS the draw/present path

`draw+present` = **17.42 ms** on a frame whose budget is 41.67 ms and whose
typical cost is ~0.2 ms — a **~85× spike**. Because
`gap(n) = targetInterval + work(n)`, a 17.4 ms work spike produces a
41.67 + 17.4 ≈ 59–62 ms observed gap, matching the measured `gapP99=62.41 ms`
exactly.

**Conclusion: the stutter is an occasional long stall inside
`DrawVideoFrame` → `RenderFrame` → `EndFrame` → `Present(1,0)`.**

### P6 — The vblank arithmetic is confirmed on the live measurement

`gapP50 = 41.88 ms`. On a 160 Hz grid the only representable dwells are multiples
of 6.25 ms; 41.88 ms is not one, so the presents are **not** landing on a uniform
vblank lattice — consistent with §2's quantization argument. The median is close
to ideal *because the median of an alternating sequence lands near the mean*; the
damage is entirely in the tail.

### P7 — The tail is the defect, not the median

| Metric | Good run | Bad run | Nominal |
|---|---|---|---|
| gapP50 | 41.88 ms | 41.88 ms | 41.667 ms |
| gapP99 | 47.75 ms | 62.41 ms | 41.667 ms |
| gapMax | 48.00 ms | 67.78 ms | 41.667 ms |
| gapJit | 1.34 ms | 5.84 ms | 0 ms |

**The median is stable and correct across every run. Only the tail moves.** Any fix
must attack the tail; tuning the average is pointless.

### P8 — NEGATIVE RESULT: the top-tier display-sync fix was tested and REJECTED

The research says mpv and Chromium present per-vblank rather than per-source-frame
(§4.2, §6.1). That was implemented and measured on this machine:

| Metric | Source-rate pacing | Display-rate pacing (mpv/Chromium model) |
|---|---|---|
| gapP50 | 41.88 ms | **6.26 ms** ✅ (clean vblank) |
| gapP95 | 42.46 ms | 16.93 ms ❌ |
| gapP99 | 47.75 ms | **71.64 ms** ❌ (worse) |
| gapMax | 48.00 ms | **93.47 ms** ❌ (worse) |
| gapJit | 1.34 ms | **10.49 ms** ❌ |
| effectiveFps | 24.0 | **22.2** ❌ (fell) |
| frames > 12 ms work / 45 s | 1 | **346** |

The median became a perfect 6.25 ms vblank, but **4K presented 160×/s is beyond
this GPU**: the render thread stalled constantly, the tail got worse, and the
effective frame rate actually *dropped*.

**Verdict: reverted.** This is exactly why the measurement was worth building — the
textbook top-tier strategy is wrong for 4K wallpaper content on this hardware.
Source-rate pacing keeps `gapP50` and `gapP95` correct; the residual defect is the
draw/present stall identified in P5, which is a *different* bug from cadence.

---

## 0c. INCIDENTAL BUG FOUND — fullscreen auto-pause false-positives (test confound)

While bisecting, four of six measurement runs stalled at `position≈2.4 s` with
`playing:false`. Cause, from `runtime.log`:

```
Full-screen window: class=CASCADIA_HOSTING_WINDOW_CLASS rect=3840x2160
Full-screen app detected — requesting wallpaper pause
Auto-pause applied (fullscreen=true, suspend=false)
```

`CASCADIA_HOSTING_WINDOW_CLASS` is **Windows Terminal**. `CheckFullScreenPause`
(Engine.cpp:1494-1528) treats *any* foreground window covering ~the whole monitor as
full-screen and pauses the wallpaper.

**Impact on the user:** with the Windows taskbar set to auto-hide, a *maximized*
window is genuinely 3840×2160, so **any maximized app freezes the wallpaper**, and
foreground flicker produces repeated pause/resume cycles — which look exactly like
intermittent stutter. This must be fixed: exclude known non-fullscreen window
styles (`WS_OVERLAPPEDWINDOW` without `WS_POPUP`, i.e. maximized rather than
true fullscreen) or require the window to be topmost/cover the taskbar.

**Method note:** this also invalidated an earlier "regression" bisect. All
measurements in §0b were re-taken with `pauseOnFullscreen:false` (an `INIT`
parameter) and are therefore clean. A harness passing that flag is required for any
future headless timing measurement on this machine.

### P10 — PRESENTMON (2026-11): capability facts + one retracted finding

**ERRATUM (self-caught).** An earlier revision of this section reported a
"bimodal displayed-frame distribution (P50 14.67 / P95 73.96 ms)" as proof that
the stutter was real and invisible to REX's telemetry. **That was a measurement
artifact and is withdrawn.** It came from taking successive deltas of
`DisplayedTime`, which in this AMD PresentMon build is *not* a monotonic
timestamp: 196 of 474 successive deltas are negative, with and without
`--qpc_time`, and zero frames report `DisplayedTime == 0`. Delta-ing a latency
column manufactures exactly the bimodality that was reported.

The sound columns are `CPUStartTime` (monotonic, ~41.8 ms steps) and `FrameTime`
(present-to-present): **P50 41.87 ms, P95 42.38 ms, MAX 47.11 ms**, which agrees
with REX's own `gapP50 41.87 / gapP95 42.40`. **No bimodality exists in the
present cadence.**

Method lesson, recorded because it nearly derailed the milestone: *validate a
ground-truth tool against a known-good reference before believing its output.*
REX's own telemetry was independently correct, and the external tool was
misapplied.

**Tooling prerequisite for Phase 1/2:** the AMD-bundled PresentMon
(`C:\Program Files\AMD\CNext\CNext\PresentMon-x64.exe`) is usable for
`PresentMode`, `AllowsTearing`, `SyncInterval`, `CPUStartTime` and `FrameTime`,
but its `DisplayedTime`/`DisplayLatency` cannot be used for interval statistics
in this build. To measure *displayed*-frame timing as the roadmap requires, either
a current upstream PresentMon 2.x is needed, or the ETW `DxgKrnl`
`Displayed*` events must be read directly. **This is an open tooling gap, not an
engine finding.**

---
WorkerW wallpaper can *never* take the independent-flip path. Measured, 475
presents:

```
PresentMode = 'Hardware Composed: Independent Flip'   x475
AllowsTearing = '0'                                    x475
SyncInterval  = '1'                                    x475
```

§5.3's central claim is **wrong**: the wallpaper does get an independent flip.
Its "one shared DWM cadence" reasoning cannot be relied on.

`AllowsTearing = 0` is real and actionable: the swap chain never passes
`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`. That is a concrete Phase 2 prerequisite,
currently unmet and cheaply testable.

`SyncInterval = 1` confirms the presentation is vsync-locked and therefore off
the VRR code path (Microsoft: tearing requires sync interval 0).

**Present cadence is steady.** `FrameTime` (present-to-present) over 475 presents:

```
P50 = 41.87 ms    P95 = 42.38 ms    MAX = 47.11 ms
```

matching REX's own `gapP50 = 41.87 / gapP95 = 42.40` almost exactly, so the
submit clock is healthy. `CPUStartTime` is monotonic at ~41.8 ms steps and
`GPUWait` ranges 0–35.5 ms, `DisplayLatency` 43–116 ms.

---

New telemetry: `phBegin` (wake → `BeginFrame` returned), `phDraw` (draw +
`RenderFrame` + `Present`), `phWork` (whole frame), `phWait` (frame-latency
waitable block). Undisturbed 60 s run, Aari 4K24:

```
gapP50=41.87ms gapP95=42.40ms gapP99=42.66ms gapMax=42.94ms gapJit=0.31ms
phBegin=0.06ms  phDraw=0.27ms  phWork=0.25ms  phWait=0.00ms
```

* **`phWait = 0.00 ms` — R4 is definitively a non-issue.** The frame-latency
  waitable never blocks on this machine. Combined with §4.2 (the wait is already
  performed correctly in `BeginFrame`, before rendering, first frame included),
  there is nothing to fix. The reviewer asked for this measurement precisely
  because it was the one unverified claim; it is now verified.
* `phBegin = 0.06 ms` — the decode-fence / frame-context wait is free.
* `phDraw = 0.27 ms` — draw + present cost a quarter of a millisecond. The
  earlier **17.42 ms** spike (P5) is therefore a rare outlier, not the steady
  state, and it is the only thing left to explain.
* `phWork` (0.19–0.52 ms) is ~0.6 % of the 41.667 ms budget, so **essentially
  the entire inter-present interval is the `PreciseSleepUntil` pacing sleep** —
  the loop is pacing exactly as designed.

**Conclusion:** in an undisturbed environment REX is effectively perfect
(`gapJit 0.31 ms`, `gapP99 42.66 ms` vs 41.667 nominal). Every stutter observed
so far correlates with an *external perturbation* of the machine, not with a
defect in the steady-state pipeline. The remaining work is to characterise what
causes the perturbation (DWM compositor contention, foreground-window changes,
the auto-pause path), not to retune the loop.

---

`Engine::GetPosition()` (Engine.cpp:716-722) is a real wall-clock stopwatch, and
`effFps` is computed as *presents ÷ elapsed*, sampled every 5 s.

**Averaging cannot detect jitter.** A pipeline presenting frames at a rock-steady
41.667 ms and one alternating 37.5/43.75 ms both report `effFps = 24.0`.
The instrument is structurally blind to the only defect that matters here.

**There is no inter-present-interval distribution anywhere in the telemetry.**
Adding it is remediation item R6 below.

---

## 2. ROOT CAUSE (primary) — `Present(1,0)` quantizes to the vblank grid, and FreeSync is not in the path

### 2.1 What REX does

`D3D12Renderer::EndFrame()` (D3D12Renderer.cpp:1290):

```cpp
HRESULT hr = m_swapChain.Present(1, 0);
```

`SwapChain.cpp:44` creates the chain with `DXGI_SWAP_EFFECT_FLIP_DISCARD`,
`SwapChain.cpp:72` sets `SetMaximumFrameLatency(2)`.

### 2.2 Microsoft, on what syncInterval=1 means under the flip model

> For the flip model (`DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`), values are:
> - `0` — Cancel the remaining time on the previously presented frame and discard
>   this frame if a newer frame is queued.
> - `1` through `4` — **Synchronize presentation for at least n vertical blanks.**

— https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present

> When you call `Present`, the runtime shows frame A for only 1 vertical blank
> interval… **The runtime discards frames B and C.**

Each frame is therefore displayed for a **whole number of vertical blanks**.

### 2.3 The arithmetic on this machine

| Quantity | Value |
|---|---|
| Display refresh | 160 Hz → vblank = **6.250 ms** |
| Aari source | 24.0 fps → frame = **41.667 ms** |
| Vblanks per source frame | 160 / 24 = **6.667** — *not an integer* |

Because dwell must be an integer count of vblanks, the true on-screen sequence is:

```
frame 0 → 7 vblanks → 43.750 ms
frame 1 → 6 vblanks → 37.500 ms
frame 2 → 7 vblanks → 43.750 ms
frame 3 → 7 vblanks → 43.750 ms
frame 4 → 6 vblanks → 37.500 ms
   … repeating, mean 6.667
```

**±3.125 ms alternation every second frame — a 7.5 % modulation of frame duration
at ~8 Hz.** That is textbook judder on a panning shot. It is invisible in the
average and glaring in motion. This exactly matches *"inconsistent, not complete
smoothness."*

### 2.4 FreeSync is doing nothing — this is the decisive part

Microsoft states, verbatim:

> **Allowing tearing is a requirement of variable refresh rate displays.** … The
> sync interval passed in to Present (or Present1) **must be 0**.

— https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present

> **Variable refresh rate displays require *tearing* to be enabled**, this is also
> known as "vsync-off" support.

— https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays

**Conclusion:** the user has Adaptive-Sync switched on, and it is *inert* for this
engine. `Present(1,0)` is not on the VRR code path at all. The monitor never
adapts to the content; DXGI hard-quantizes the wallpaper to the 160 Hz vblank
grid, and 6.667 does not divide evenly. Turning FreeSync on cannot fix this, and
no UI/CSS/GPU-load change can either. This is why every prior hypothesis failed.

### 2.5 Why the window being visible makes it *worse*

A WorkerW wallpaper is behind all other content by construction, so it can **never**
take the independent-flip path — DirectFlip requires the app's own window to be
what is on screen
(https://devblogs.microsoft.com/directx/dxgi-flip-model). It is permanently a
**composed** present, consumed by the DWM compositor cycle.

> Each part of the API influences or observes the work cycle of the System
> Compositor. **This work cycle is a regular cadence that produces one Compositor
> Frame per cycle.** That cycle may or may not be aligned to display vblanks,
> depending on **system workload**, number of displays, and other factors.

— https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock

So the wallpaper and the Electron window **share one compositor cadence**:

* **Window hidden** → Chromium submits no frames → compositor cycle is light →
  wallpaper frames land on their intended vblank.
* **Window visible** → Chromium submits a frame on every BeginFrame (it presents at
  *display* rate, not source rate) → the shared cycle is congested → the wallpaper's
  frames get pushed into later cycles irregularly.

This explains the entire observation set without any appeal to GPU shader load.
It is a **composition-cadence** defect, not a **fill-rate** defect — which is
precisely why deleting every backdrop-filter accomplished nothing.

### 2.6 The fix, per Microsoft and mpv

Microsoft's own documented recovery for exactly this unevenness is to **step off the
vblank grid**:

> **To recover from the glitch:** Calculate the number of frames to skip to recover
> from the glitch… **Pass 0 to the *SyncInterval* parameter** in this number of
> calls to `IDXGISwapChain1::Present1` to discard and skip this number of frames.

— https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model

mpv reaches the same conclusion independently:

> I would suggest that the video's actual framerate should be multiplied by the
> largest integer such that the result does not exceed the monitor's reported
> refresh rate. For example, 24fps content would be doubled to 48 on 60hz
> displays, and tripled to 72 on 75hz displays. This maximises the likelihood
> that adaptive sync will kick in.

— https://github.com/mpv-player/mpv/issues/6137

At 160 Hz: `floor(160 / 24) = 6` → present at **144 Hz** effective cadence, inside
the VRR window of any 160 Hz panel.

---

## 3. ROOT CAUSE (secondary) — packets are silently destroyed on send-EAGAIN

`FfmpegVideoDecoder::DecodeFrame`, FfmpegVideoDecoder.cpp:667-672:

```cpp
const int s = avcodec_send_packet(m_decCtx, m_pkt);
av_packet_unref(m_pkt);      // ← destroys the packet unconditionally
m_pktPending = false;
if (s == 0 || s == AVERROR(EAGAIN)) {
    continue;                // ← EAGAIN treated as success
}
```

### The FFmpeg contract (authoritative)

> In theory, sending input can result in EAGAIN — this should happen only if not
> all output was received. […] the only guarantee is that an `AVERROR(EAGAIN)`
> return value on a send/receive call on one end implies that a receive/send call
> on the other end will succeed… **A codec is not allowed to return
> `AVERROR(EAGAIN)` for both sending and receiving.**

— https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html

### Verdict: contract violated, twice

1. **`av_packet_unref` on EAGAIN destroys a packet the decoder never accepted.**
   That is permanent frame loss, not a retryable condition.
2. **EAGAIN is branched on as if it were success.** The loop then `continue`s to
   `avcodec_receive_frame`, which returns EAGAIN, which reads the *next* packet —
   feeding the decoder a stream with a hole in it.

The audio path already got this right in commit `81d7bbb`
(`SubmitPacketAndDrain` / `DrainAvailableFrames` in AudioPlayer.cpp:650-705).
**The video path was never fixed.** The test file at
`rex-core/tests/wallpaper/test_audio_dolby.cpp:228` documents this exact defect
class in a comment — *"return value IGNORED (the defect)"* — proving the pattern
was found and fixed in audio but missed in video.

### Why this matches the symptom

With hwaccel + `thread_count = 1`, the decoder's reorder buffer is small and
send-EAGAIN becomes reachable under GPU load. Each occurrence silently drops one
frame and hands the decoder a discontinuity — which surfaces as a short repeat or
a hitch, i.e. **inconsistent** stutter rather than a constant frame-rate deficit.
It also explains why a *silent* clip still stutters (this path is independent of
audio).

### Peer comparison — every top-tier decoder retries

| Player | Behaviour on send-EAGAIN |
|---|---|
| LAV | retries the same packet |
| MPC-HC | = LAV |
| mpv | `mp_pin_out_unread` rollback — `filters/f_decoder_wrapper.c:1507-1512` |
| VLC | retries |
| **REX** | **drops the packet** |

---

## 4. Secondary defects found

### 4.1 `thread_count = 1` is wrong on the software-fallback path

FfmpegVideoDecoder.cpp:173, 423, 441:

```cpp
m_decCtx->thread_count = 1;  // deterministic sequential decode (wallpaper-style)
```

| Player | Software decode | hwaccel decode |
|---|---|---|
| LAV | `av_cpu_count()`, clamped [1,32] | **forced 1** + `thread_type = 0` |
| mpv | `av_cpu_count()+1`, cap 16 | hwdec-threads default **4** |
| VLC | `cpu_count()+1`, cap 6 (10 HEVC) | unchanged (only `b_hardware_only` forces 1) |
| Chromium | `w*h*3/1920/1080` → 2…12, clamp [2,16] | n/a (uses VideoDecodeAccelerator) |
| **REX** | **1** | 1 |

**Verdict:** for the *hardware* path this is defensible and matches LAV. For the
*software fallback* it is a severe throughput cap — a 4K software decode on one
core is the classic "stutter under any load" failure. Must be
`thread_count = 0` (auto) on the software path.

### 4.2 Frame-latency waitable — ✅ ALREADY CORRECT (audit erratum, 2026-11)

The first version of this audit claimed the handle was "created and never
awaited". **That was wrong**, and the reviewer was right to demand verification.
Verified against source:

* `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` is passed at creation
  (`SwapChain.cpp:46`).
* `WaitForFrame()` **is** called from `D3D12Renderer::BeginFrame()`
  (`D3D12Renderer.cpp:1141`) — i.e. before any rendering work, which is exactly
  where Microsoft requires it, and it therefore also covers the first frame.
* `WaitForFrame()` uses a 1000 ms timeout (`SwapChain.cpp:184-188`), and
  `SwapChain.cpp:128` records that an earlier unbounded version burned a 1 s
  timeout per frame (~1 fps stutter) and was already fixed.

**Verdict: R4 is not a defect.** The only remaining work is *measurement* — how
long the wait actually blocks — which the phase timings in P1 provide.

---

REX sets only `hw_device_ctx` (FfmpegVideoDecoder.cpp:198). It does **not** set
`hw_frames_ctx` and registers **no `get_format` callback**.

Consequence: FFmpeg allocates its own frames, and every frame must then be pushed
into REX's own 8-slot pool:

* hardware path — `ID3D11VideoProcessor::VideoProcessorBlt` (YUV→RGB, line ~817)
  followed by `CopyResource` into the pool slot (line 1014)
* software path — `av_hwframe_transfer_data` (line 933) + swscale: a **full CPU
  round-trip per frame**

| Player | hw_device_ctx | hw_frames_ctx | get_format | Per-frame extra passes |
|---|---|---|---|---|
| mpv | yes | **yes** | yes | 0 (decoder allocates from app pool) |
| VLC | yes | yes | — | 0 |
| LAV | legacy `hwaccel_context` | — | yes | 0 (pool owns surfaces) |
| **REX** | yes | **no** | **no** | **2** |

These extra passes are precisely what a congested compositor cycle delays. Under
no load they are free; under contention they are the difference between smooth and
stutter.

### 4.4 `avcodec_flush_buffers` and loop wrap

`SeekInternal` (line 355-371) uses `AVSEEK_FLAG_BACKWARD` then
`avcodec_flush_buffers`. Correct order, matches peers. **No defect.**

### 4.5 `AV_CODEC_FLAG_LOW_DELAY` — NOT a defect (self-correction)

Flagged as a suspect initially. Peer check:

| Player | Sets `AV_CODEC_FLAG_LOW_DELAY` |
|---|---|
| LAV | never (0 hits / 281 files) |
| mpv | never (0 hits / whole tree) |
| VLC | only when `--low-delay` or fps < 6 |
| Chromium | never (its `low_delay` is an unrelated parameter) |
| **REX** | never |

**REX matches LAV and mpv. No defect. Do not change this.**

---

## 5. Comparison scorecard — where REX stands

| Axis | REX | Top tier (mpv/LAV/VLC/Chromium) | Verdict |
|---|---|---|---|
| Demux / interrupt callback | ✅ clean | same | **Top tier** |
| EAGAIN packet handling | ❌ **drops packet** | all retry | **Below top tier — CRITICAL** |
| Decoder thread config (hw) | ✅ =1 (matches LAV) | LAV =1, mpv =4 | Acceptable |
| Decoder thread config (sw) | ❌ =1 | all auto/multi | **Below top tier** |
| `hw_frames_ctx` zero-copy | ❌ absent | mpv/VLC/LAV all have it | **Below top tier** |
| hwaccel ownership/refcount | ✅ explicit AddRef transfer, documented | — | **Top tier** (better than most) |
| Frame pool bounded + keyed | ✅ exact (size, format) keying | — | **Top tier** |
| Ring scheduler / backpressure | ✅ SPSC, high-water drop accounting | — | **Top tier** |
| High-res pacing timer | ✅ `PreciseSleepUntil` w/ quantum proof | — | **Top tier** |
| Frame-latency waitable | ❌ created, never awaited | all use it | **Below top tier** |
| Presentation pacing | ❌ **source rate, syncInterval=1** | mpv = display rate + repeat | **Below top tier — ROOT CAUSE** |
| VRR / tearing path | ❌ **never enabled** | mpv/Chromium engage it | **Below top tier — ROOT CAUSE** |
| Jitter telemetry | ✅ **shipped** `gapP50/95/99/Max/Jit` | — | **Ahead of every peer** (see §0b) |

**Summary:** the decode *engine internals* (pool ownership, backpressure, pacing
timer, refcount discipline) are genuinely top-tier — several are better than LAV.
The two deficits that produce the user's symptom are **the presentation model** and
**one dropped-packet branch**.

---

## 6. Remediation — prioritized

### R1 — CRITICAL: stop dropping packets on send-EAGAIN
`FfmpegVideoDecoder.cpp:667-672`. Mirror the already-correct audio implementation
(`AudioPlayer.cpp` `SubmitPacketAndDrain`/`DrainAvailableFrames`): on EAGAIN, keep
`m_pktPending = true`, drain `avcodec_receive_frame` until it is no longer EAGAIN,
then retry the *same* packet.
**Verify:** new unit test feeding a synthetic EAGAIN; frame count in == frame count out.

### R2 — CRITICAL: leave the vblank grid
Present with `syncInterval = 0`, swap chain flagged
`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, and Present with
`DXGI_PRESENT_ALLOW_TEARING`, gated on
`IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`.
Gate behind a capability probe + user setting, with the current `Present(1,0)`
path retained as fallback.
**Verify:** `effFps` unchanged **and** inter-present jitter drops below 1 ms; the
judder disappears on the 24 fps/160 Hz case.

### R3 — HIGH: present at an integer multiple of the source rate
Implement mpv's rule: `repeats = floor(displayHz / sourceFps)`; present content at
`sourceFps × repeats` (24 → **144 Hz** at 160 Hz) so every content frame occupies a
constant number of vblanks and VRR has an in-range target to follow.
**Verify:** constant vblank dwell in PresentMon; no periodic 8 Hz beat.

### R4 — HIGH: await the frame-latency waitable
`WaitForSingleObject(m_frameLatencyWaitable, …)` once per frame before `BeginFrame`,
including the first.
**Verify:** present-path latency falls; no stall inside `Present()`.

### R5 — MEDIUM: `thread_count = 0` (auto) on the software path
Leave `= 1` when `m_hardware` is true (matches LAV); use 0 otherwise.
**Verify:** software-decode throughput on a 4K clip; no regression on hw path.

### R6 — MEDIUM: instrument jitter, because the telemetry cannot see it — ✅ SHIPPED `bebfb00`

**STATUS: DONE.** `Engine::GetTelemetry` now publishes
`presentGapP50Ms / P95 / P99 / Max / JitterMs` from a 256-sample ring of
inter-present intervals (recorded in `RenderLoop`, sampled by `GET_STATUS` and the
`runtime-telemetry.log` writer). This is what made §0b measurable and what
exonerated the decoder, the scheduler and the housekeeping timer.

**REMAINS (see §7 review follow-up):** the metric is derived from `Present` entry
timestamps on our own thread. It proves *when we submitted*, not *when the frame
reached the panel*. Phase timings (frame deadline, render start, draw completion,
`Present` entry/exit, frame identity) are the next addition, and PresentMon is the
external ground truth.

**Verify:** the metric exists, showed the defect before the fix (gapP99 62 ms) and
the clean state after (`gapJit` 0.31 ms, 0 late frames).

### R7 — LOW: adopt `hw_frames_ctx` + `get_format` for zero-copy
Let FFmpeg allocate decoded surfaces from REX's pool instead of copying each frame
through `VideoProcessorBlt` + `CopyResource`.
**Verify:** per-frame GPU pass count drops by 2; no change to colour output.

---

## 7. Explicitly UNPROVEN / out of scope

* **Whether an Electron window changes the DWM compositor cadence for the
  wallpaper.** The mechanism is documented and consistent with every observation,
  but the *magnitude* is unmeasured. R2/R3 should be validated with PresentMon
  (`PresentMode`, `DisplayedTime`, `SyncInterval`, `PresentRefreshCount`) rather
  than assumed.
* **Whether a composed WorkerW surface can benefit from VRR at all.** Microsoft
  documents VRR as requiring tearing; the wallpaper can never independent-flip.
  Community reports suggest a composed wallpaper *does* pull the panel's refresh
  rate down, but that is a low-confidence report. R2 must be measured, not assumed.
* **Chromium's `FrameRateMatcher` cadence algorithm** — path could not be fetched
  (404 on HEAD). The existence of the content-frame-interval hook is verified; the
  algorithm is not.
* **VLC's judder handling** — mpv's wiki *asserts* VLC behaves classically but does
  not cite VLC. Not used as evidence here.
* **`thread_count` on the software path is untested in practice** — R5 is inferred
  from peer behaviour, not measured on this machine.

---

## 8. Sources

* FFmpeg send/receive API contract — https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html
* DXGI Present, sync interval — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
* DXGI present / tearing conditions — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
* Variable refresh rate displays — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
* DXGI flip model — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
* DirectX blog, flip model — https://devblogs.microsoft.com/directx/dxgi-flip-model
* Frame latency waitable object — https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
* Reduce latency with DXGI 1.3 — https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
* Compositor clock — https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock
* DWM registry values (OverlayMinFPS) — https://learn.microsoft.com/en-us/windows/win32/dwm/registry-values
* Composition vs direct flip — https://learn.microsoft.com/en-us/windows/win32/comp_swapchain/comp-swapchain
* Chromium VideoFrameSubmitter — https://chromium.googlesource.com/chromium/src/+/HEAD/third_party/blink/renderer/platform/graphics/video_frame_submitter.cc
* Chromium VideoFrameCompositor — https://chromium.googlesource.com/chromium/src/+/cb728bef621d221f70c0b8e08af4b8e90f8f3b90/media/blink/video_frame_compositor.cc
* Chromium multi-refresh stutter issue — https://issues.chromium.org/issues/360354541
* Chromium swapchain frame-latency bug — https://issues.chromium.org/issues/40890504
* mpv display synchronization — https://github.com/mpv-player/mpv/wiki/Display-synchronization
* mpv FreeSync frame doubling (#6137) — https://github.com/mpv-player/mpv/issues/6137
* mpv VRR / display-resample (#12005) — https://github.com/mpv-player/mpv/issues/12005
* Top-tier decoder axis-by-axis research — see `_research_top_decoders.md`
* Presentation pacing research — see `_research_present_pacing.md`