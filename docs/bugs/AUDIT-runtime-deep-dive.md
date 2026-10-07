# AUDIT-runtime-deep-dive — wallpaper runtime implementation audit (2026-10-07)

> Method: 5 parallel read-only audits (command protocol / decode pipeline /
> playback integration / present+pacing / completeness gaps), then **manual
> re-verification of every high-severity claim** against the cited lines.
> **V** = hand-verified · ⚠️ = agent-reported, plausible, not re-read ·
> H = hypothesis/inference. No source files were modified for this audit.

Related: [`AUDIT-2026-10-06-parallel-audit.md`](AUDIT-2026-10-06-parallel-audit.md) (first audit),
[`BUG-001-workerw-zorder-input-hang.md`](BUG-001-workerw-zorder-input-hang.md).

---

## R1. CRITICAL · V — Hot-reload `LoadTexture()` deadlocks the render thread inside `Begin/End`

`Engine.cpp:1427` calls `m_renderer->LoadTexture(renderHotReloadPath)` between
`BeginFrame()` (`:1393`, which does `m_renderMutex.lock()` —
`D3D12Renderer.cpp:1103`) and `EndFrame()` (`:1449`). `LoadTexture()` takes
`std::lock_guard(m_renderMutex)` (`D3D12Renderer.cpp:618`). Same thread +
non-recursive `std::mutex` = self-deadlock: editing a displayed static
wallpaper hangs the render thread holding the lock, and the command thread
then blocks forever on the next setter. (`UpdateTexturePixels` explicitly
avoids this with a no-lock comment — `LoadTexture` was never given the same
treatment.)
Fix direction: move the hot-reload block before `BeginFrame()`, or add a
`LoadTextureBetweenFrames`-style entry point that assumes the lock is held.

## R2. HIGH · V — Static scenes present at full rate forever (P16 defeated)

The only `m_frameDirty.store(false)` in `Engine.cpp` is at `:1404`, inside
`if (pending && m_renderer)` — and `pending` is non-null only for video
(`:1380-1383`). A static image therefore never disarms the flag the setters
arm (`:527,684,702,720,738`) → the gate at `:1387` passes on every wake →
`Present` at display rate + unbounded `m_presentCount` growth for a still
picture. Fix direction: clear the flag after any presented frame (move the
store out of the video-only branch), and arm it from `ApplyTransform`
(rotation/flip currently don't — latent same-shape bug once this is fixed).

## R3. CRITICAL · V — Software decode uploads RGB bytes into a YUV pool (corrupt frames)

`EnsurePool` builds `m_hdrContent ? P010 : NV12` (`FfmpegVideoDecoder.cpp:708-709`)
while `ConvertSoftware` produces `swFormat = m_hdrContent ? RGBA64 : BGRA`
(`:878`) and `memcpy`s it through staging into the pool slot (`:935-949`).
The comment at `:705-707` promises an `RGBA16_UNORM` pool for HDR+SW that was
never implemented. Any software-decoded frame (HW fallback path) is corrupt.
Fix direction: implement the promised pool format branch (and matching shader
input), or convert SW output to NV12/P010 instead.

## R4. CRITICAL · V — `EnsurePool` fast-path requires `m_processor`, null on software decode → pool rebuilt every frame

`FfmpegVideoDecoder.cpp:683-687` requires `m_processor` for the fast path, but
it is only created `if (m_hardware)` (`:742-760`). `ConvertSoftware` calls
`EnsurePool` per frame (`:913`), so software decode does
CloseHandle+CreateTexture2D+CreateSharedHandle ×8 **per frame**, and the new
handles miss the renderer's 8-slot shared-handle cache
(`D3D12Renderer.hpp:221`, `kMaxVideoSlots = 8`) → cache exhaustion → video
freezes. Fix direction: drop `m_processor` from the fast-path condition (or
track pool validity separately).

## R5. HIGH · V — 0-frame file with `loop=true` hangs `Open` forever; failed seek + loop busy-spins

`DecodeFrame` EOF path (`FfmpegVideoDecoder.cpp:593-602`): drain-EOF →
`SeekInternal(0.0)` succeeds → `continue` → `av_read_frame` EOF again
(`m_eof` was reset by the seek) → cycle never returns. The prewarm calls it
synchronously under `Open`, so a 0-frame file hangs `LOAD`. If the seek fails
instead, `DecodeFrame` returns false but `DecoderThread:280-282` does
`if (GetLoop()) continue;` with no sleep → 100% CPU spin. Fix direction: bound
the EOF→seek→EOF cycle (return false after N consecutive EOFs), sleep in the
loop-on-failure path.

## R6. HIGH · V — Audio ring overflows on multichannel sources

`AudioPlayer::PushFrameToRing` (`AudioPlayer.cpp:331-352`): the ring is
allocated for `m_channels`, but stride and memcpy size use
`channels = max(frameCh, m_channels)` (`:333-334,343,352`). A 5.1/quad frame
on a stereo ring writes past the allocation (heap overflow) and corrupts the
layout the reader (`:432-435`, `m_channels` stride) expects. Fix direction:
stride/clamp everything by `m_channels` (downmix), never by the frame count.

## R7. HIGH · V — `SwapChain::Resize` strips `FRAME_LATENCY_WAITABLE_OBJECT`

`Initialize` creates the swapchain with the flag (`SwapChain.cpp:46`) and
caches the waitable; `Resize` calls `ResizeBuffers(..., 0)` (`:126-130`) which
clears it (the HDR path's internal resize at `:186-187` preserves it). After
any display-change resize, `WaitForFrame()` waits on a dead handle → 1 s
timeout every frame → ~1 fps stutter. Fix direction: pass the flag (and
re-query the waitable) in `Resize`.

## R8. HIGH · V — `Stop()` splits A/V + position: audio rewinds, video and clock don't

`VideoPlaybackController::Stop` (`:145-152`) stops/pauses and zeroes
`m_position` but never seeks the decoder; `AudioPlayer::Stop`
(`AudioPlayer.cpp:143-147`) rewinds to 0 and requests seek. `Play()` only
re-seeks when `position >= duration - 0.001` (`:126-129`) — and `Stop()` just
zeroed it — so after `Stop(); Play()` audio restarts at 0 while video resumes
mid-stream, and `Engine::Stop` (`Engine.cpp:567-568`) zeroes the wall clock
while the decoder keeps its PTS. Fix direction: `Seek(0.0)` (decoder + audio +
scheduler clear) inside controller `Stop()`.

## R9. HIGH · V — `LOAD` applies loop+speed but drops volume/mute; recovery unmutes

`Engine::LoadScene` success path pushes only `SetLoop` + `SetSpeed`
(`Engine.cpp:427-428`); `SetVolume/SetMute` (`:743-761`) store into the scene
but are never re-pushed after `m_currentScene = scene` (`:373`), and
`HandleDeviceLoss` reloads through the same `LoadScene` (`:335`) — so every
recovery also unmutes/resets volume. Per-monitor controllers have loop/speed
setters but no volume/mute equivalent at all (`VideoManager.cpp:223-229`).
Fix direction: re-push all four knobs after load, add per-monitor volume/mute.

## R10. HIGH · V — `Seek` never clears `FrameScheduler`: stale pre-seek frame presented

`VideoPlaybackController::Seek` (`:180-194`) seeks the decoder + audio but
`FrameScheduler` has only `Push`/`Tick` (latest-wins, `FrameScheduler.cpp:11-18`),
and the decoder flush doesn't touch it. The renderer adopts a frame whose
PTS/slot predates the seek until the decoder produces a new one. Same path
covers loop wrap (`FfmpegVideoDecoder.cpp:595`) and recovery seek
(`Engine.cpp:343`). Fix direction: add `FrameScheduler::Clear()` and call it
in `Seek`, on loop wrap, and before recovery resume.

## R11. MAJOR · V — Unhandled `std::filesystem::exists` throw crashes the runtime on hostile paths

`wallpaper_cli_main.cpp:815` calls the throwing single-arg `exists()` with no
`try/catch` anywhere in `RunRuntime` (only the `wmain` terminate handler).
Over-long paths, reserved device syntax, or dead network shares that fail the
OS status call throw `filesystem_error` → uncaught → `terminate`/abort instead
of `ok:false "file not found"`. Fix direction: `std::error_code` overload +
length cap before conversion. (The adjacent `Utf8ToWiden` SIZE_MAX theory was
checked and downgraded: with `dwFlags=0` invalid UTF-8 substitutes U+FFFD
rather than failing — still harden it with `MB_ERR_INVALID_CHARS`.)

## R12. MAJOR · V — Zero-length frame is conflated with EOF: one bad frame kills the session silently

`ReadFrame` returns false for `len == 0` (`wallpaper_cli_main.cpp:508`) and the
dispatch loop `while (ReadFrame(frame))` (`:707`) exits into the stdin-closed
cleanup — engine torn down, no `ok:false` ever sent, client sees only
timeouts. A zero-length prefix is a protocol violation, not EOF. Fix
direction: answer `malformed frame` and continue (mirror the oversized-frame
recovery posture).

## R13. MAJOR · V — `DuplicateHandle` results ignored; stale client process never revalidated

`wallpaper_cli_main.cpp:1056-1076`: fence + slot duplications discard the
`BOOL` return, then unconditionally mark `duplicated = true` — a dead client
yields NULL/stale handles serialized as valid. `CloseClientSurface` never
touches `pid/process`, `SHUTDOWN`/EOF leak it, there is no liveness check
(`WaitForSingleObject(process,0)`) and no explicit detach. Fix direction:
check every duplication, `CloseClientSurface()` + `ok:false` on failure,
liveness gate, detach command.

## R14. MAJOR · ⚠️ — `Close()` can hang in blocking `av_read_frame` (no interrupt callback)

`DecoderThread` blocks in `av_read_frame` (`FfmpegVideoDecoder.cpp:609`);
`Close` clears the flag and `join()`s (`VideoPlaybackController.cpp:101-107`).
Repo-wide grep found no `interrupt_callback`/`AVIOInterruptCB`, so slow/network
files stall teardown until the FFmpeg timeout. Same shape in `AudioPlayer`'s
network reads (unverified which files). Fix direction: `AVIOInterruptCB`
wired to the stop flag.

## R15. MAJOR · ⚠️ — `Seek` vs `Close` race on `m_decoder`; unlocked raw reads in diagnostics

`VideoPlaybackController::Seek/SelectBestStream/GetSlotHandle` dereference
`m_decoder` under only `m_decodeMutex` (or none) while `Close` resets it under
a different mutex — concurrent Seek+Close is a unique_ptr race/UAF. Inside the
decoder, `GetDiagnostics` reads raw `m_hardware`/`m_eof` written by the decode
thread with no lock. Fix direction: single mutex (or shared_ptr) for decoder
lifetime; atomics for the flags.

## R16. MAJOR · ⚠️ — HDR output state is snapshot-once: stale PSO, late `SetForceSdr`, no mid-session toggle detection

`m_isHdrOutput` is cached at `Initialize` (`D3D12Renderer.cpp:374,506`) and
drives PSO choice (`:1169`) while `DrawVideoFrame:846` reads live HDR state —
mismatched tone-map after any change; `m_pipelineStateHdr` is null when
starting SDR so a later HDR switch has nothing to bind. `SetForceSdr` arrives
after `Initialize` (`Engine.cpp:111,117`) while the swapchain pre-init forward
already ran — swapchain stays R16F/PQ with the shader forced SDR. An OS HDR
toggle with unchanged resolution never triggers `Resize`, so
`UpdateHdrSelection` never re-runs. Fix direction: refresh HDR state +
(pool of) PSOs on every topology event, order `SetForceSdr` before init, poll
or subscribe to HDR changes.

## R17. MAJOR · ⚠️ — Single command allocator reset per frame with no fence gating

`BeginFrame` unconditionally `Reset()`s the sole `m_commandAllocator`
(`D3D12Renderer.cpp:1102-1108`); `WaitForFrame` waits only the DXGI latency
waitable (1 s timeout, then proceeds anyway) and the per-frame fence from
`EndFrame()->Signal()` is never waited on. A GPU hitch >1 vsync resets the
allocator while the prior frame is in flight (spec violation, corruption on
real drivers). Fix direction: allocator ring ≥2 indexed by completed fence
value, or gate `Reset()` on the queue fence. (Overlaps first-audit finding A2
of `AUDIT-2026-10-06`, kept here for the present-path context.)

## R18. MEDIUM · V — `PaceLoop` ignores decoder cadence; idle loop wakes at `maxFps`; IPC `maxFps` unclamped

The header/comment promise "slower of maxFps and decoder cadence, floored at
~15 Hz" (`Engine.hpp:81-82`, `Engine.cpp:1497-1499`); the code uses only
`max(1000/maxFps, 4.0)` (`:1504`) — 4 ms = 250 Hz, and gated wakes re-arm from
`frameStart`, so an idle engine still wakes 60×/s. `maxFps` from IPC is
unclamped (`wallpaper_cli_main.cpp:748`; CLI path clamps) — `maxFps=100000`
spins at 250 Hz. Fix direction: implement the documented cadence min + floor,
clamp IPC `maxFps` to a sane range.

## R19. MEDIUM · V — Hot-reload + spectrum blocks starve behind the P16 gate

Hot-reload `%60` (`Engine.cpp:1449-1451`) and the audio-spectrum fetch
(`:1469-1474`) sit *inside* the dirty branch, after the gate's `continue`
(`:1415-1418`). Static idle → gate skips → file edits never reload, and
audio-reactive on non-video scenes never presents. Fix direction: move both
blocks before the gate (like the `%30/%120` checks).

## R20. MEDIUM · V — `SetSpeed` never reaches audio; `Stop`/`Play` position model splits further

`VideoPlaybackController::SetSpeed` (`:164-166`) only paces the video loop;
`AudioPlayer` has no `SetSpeed` at all (`AudioPlayer.hpp:58-65`), and
`VideoManager::SetSpeed` (`:219-221`) forwards only to the controller — any
speed ≠ 1.0 desyncs picture and sound by construction. Related: `Engine`
position is wall-clock, never decoder PTS — it drifts on speed change, grows
past `duration` across loops, and `HandleDeviceLoss` seeks the video to
`savedPosition` while `m_positionBase` is untouched (position ~0 over a
mid-stream picture). Fix direction: audio tempo (resample) or documented
video-only speed; reconcile engine position with decoder PTS (or at least
modulo + re-base on seek/recovery).

---

## Completeness gaps (production requirements, ranked by user impact)

### G1. No power awareness (battery/display-off/lid) — MISSING · V (absence verified by grep)
No `RegisterPowerSettingNotification`, `GetSystemPowerStatus`, or Electron
`powerMonitor` usage anywhere in `rex-core/src` or `app/electron`. Laptops
decode/upload/present at full cadence on battery; desktops burn GPU with the
display off. Only knob is `pauseOnFullscreen` (foreground windows, not power).
Fix: power/battery-saver/display-state notifications → pause decode + stop
presents, resume on power.

### G2. No session-lock pause — MISSING · V (absence verified by grep)
No `WTSRegisterSessionNotification` / `WM_WTSSESSION_CHANGE` anywhere. On
Win+L the decode + render loops keep presenting behind the lock screen at
full rate (occlusion is log-only, `D3D12Renderer.cpp:1258-1268`). Fix: session
notifications → pause/resume like fullscreen-pause.

### G3. Display-topology holes — PARTIAL · V
`CheckDisplayChanges` only fires on count + primary W/H (`Engine.cpp:1088-1090`):
secondary-only moves, same-resolution primary swaps, and DPI changes (hardcoded
`dpiScale = 1.0`, `Engine.cpp:656-658`) never reconcile; an HDR toggle with
unchanged resolution never re-runs `UpdateHdrSelection` (only called from
`Initialize`/`Resize`). Fix: widen the predicate, read real DPI, re-evaluate
HDR on any topology event.

### G4. Audio endpoint loss permanent + layered videos mix — PARTIAL · V
No `IMMNotificationClient`, no `AUDCLNT_E_DEVICE_INVALIDATED` handling:
unplug mid-playback silences wallpaper audio until reload
(`AudioPlayer.cpp:408-423` returns silently, thread never re-enumerates).
Every open per-monitor controller owns an `AudioPlayer`
(`VideoManager.hpp:223-229`) ticked per frame (`VideoManager.cpp:341-361`) —
two videos = two simultaneous WASAPI streams, no policy. Fix: endpoint
re-enumeration; single-audio-owner policy.

### G5. No protocol versioning + no second-runtime guard — MISSING · V (absence verified by grep)
No `PROTOCOL_VERSION` anywhere; skew fails as opaque `ok:false`/silent
defaults. No named mutex/lockfile — two `rex-wallpaper-runtime.exe` fight
over the WorkerW layer (Electron's single-instance lock covers only the app).
Fix: `INIT {protocolVersion}` handshake + named-mutex ownership.

### G6. Resource bounds — PARTIAL · ⚠️
No declared limits (size/resolution/duration); 8K attempts ~400 MB of shared
textures and surfaces only at first frame (`LOAD` can report success first).
OOM paths mostly return graceful errors. Compositor stall has no isolation:
`PresentToAllMonitors` runs synchronously per compositor on the render
thread — one hung swapchain stalls every monitor.

### G7. Crash containment / user surfacing — PARTIAL · ⚠️
Renderer crash is process-fatal by design (max diagnostics, recovery one level
up in Electron with capped rounds — verified working). Corrupt files fail
`LOAD` cleanly. Weak link is surfacing: `{ok:false, error}` reaches the
renderer process, which only `console.warn`s — no toast/banner, just a tray
flag and an unchanged desktop.

### G8. Observability gaps — PARTIAL · V
Telemetry is genuinely good (decode P50/P95/P99, drops, GPU budget, adapter
topology via `GET_STATUS`). Missing: per-stage drop attribution, per-compositor
counters, audio underrun state, present-latency/occlusion counters — and
`runtime.log`/`runtime-telemetry.log` grow forever (append-only; only
`crash.log` is capped and only `rex-app.log` rotates).

---

## Deliberately NOT filed (checked, not bugs)

- `ReadExact` partial-read loop, `uint32→size_t` widening, JSON nesting cap,
  monotonic request IDs (late responses can't misresolve), `SHUTDOWN`/EOF
  join order, stdout cleanliness in `--runtime` mode — all verified correct.
- `1000.0/m_frameRate` divisions guarded; SDR 1000-nit defaults ignored by the
  shader per design; HW padding cropped via `m_videoUvScale`.
- Scene-switch ordering / half-torn LOAD / rapid LOAD / UNLOAD-while-paused:
  `LoadScene` rejects non-Initialized/Stopped, holds `m_mutex` across the
  switch, joins the decode thread before reopen — no tear found.
- `ImageLoader` thread joined; mid-write failures keep the old entry and
  retry; rapid writes coalesce.
- `Present(1,0)` vsync params legal; `DXGI_STATUS_OCCLUDED` throttled
  success-noop; `DEVICE_REMOVED/RESET` → paced recovery; Begin/End pairing
  holds on the recovery/`%30`/`%120` paths.
- Rotation metadata (portrait video sideways): real but out of scope for this
  pass; note for a media-correctness sweep.
- Cloaked/secure-desktop/UIPI edge cases in fullscreen detection: hypothesis
  only, needs a live desktop to verify.
