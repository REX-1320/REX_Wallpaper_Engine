#include "wallpaper/playback/VideoPlaybackController.hpp"

#include <shared/Logger.hpp>
#include <shared/PreciseSleep.hpp>
#include <shared/ThreadName.hpp>

#include <chrono>
#include <cstdlib>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "VideoPlaybackController";

// Backlog threshold: the published content is this far behind the wall
// clock, and the renderer is allowed to jump the content forward (the
// scheduler's catch-up) instead of replaying the backlog at the renderer's
// rate. Measured collapse: reserve -3831 ms, 2-3 fps, ~90 s to unwind.
constexpr double kBacklogRecoveryMs = 1000.0;
using ms = std::chrono::duration<double, std::milli>;
double NowSec() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

VideoPlaybackController::VideoPlaybackController() {
    m_fpsStartTime = Clock::now();
}

VideoPlaybackController::~VideoPlaybackController() {
    Close();
}

shared::Result<void> VideoPlaybackController::Open(std::string_view path,
                                                   bool useHardware,
                                                   ID3D11DeviceContext* context11,
                                                   HANDLE fenceHandle) {
    std::lock_guard lock(m_threadMutex);
    if (m_open.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "controller already open");
    }

    // Reuse the closed decoder from the previous session when possible: it
    // keeps its shared-texture pool through Close(), and the D3D12 renderer
    // keeps the matching imports, so a same-generation reopen costs no new
    // driver import footprint. A decoder left open by an abnormal path is
    // discarded instead of being reused.
    std::shared_ptr<FfmpegVideoDecoder> decoder;
    {
        std::lock_guard plock(m_decoderMutex);
        decoder = m_decoder;
    }
    if (decoder && decoder->IsOpen()) decoder.reset();
    if (!decoder) decoder = std::make_shared<FfmpegVideoDecoder>();

    shared::Result<void> result = decoder->Open(path, useHardware, context11, fenceHandle);
    if (result.IsError()) {
        return result;
    }

    {
        // Publish under both mutexes (op, then leaf): a concurrent
        // Seek/Close snapshots either null or the fully opened decoder.
        std::lock_guard lock(m_decodeMutex);
        std::lock_guard plock(m_decoderMutex);
        m_decoder = decoder;
    }
    m_fenceHandle = fenceHandle;
    m_duration  = m_decoder->GetDuration();
    m_frameRate = m_decoder->GetFrameRate();
    m_contentStart = m_decoder->GetStartTime();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    {
        // P0-2: a reopen must not inherit the previous file's content clock.
        std::lock_guard clk(m_contentMutex);
        m_contentMeter.Reset();
    }
    // A reopen must be playable: clear any EOS latch from the previous file.
    m_eosReached.store(false, std::memory_order_release);
    // A reopen is a new scene: the previous scene's end must not be
    // reported for it (the app would change the wallpaper immediately).
    m_sceneEnd.Rearm();
    // Policy refresh centralised (audit 5.4 #13): scale the reserve marks
    // to what the 8-slot pool can actually hold at this source rate, then
    // apply the env overrides.
    RefreshDecodeAheadPolicy(/*logOverrides=*/true);
    // Decode pacing servo (default on): cancels the steady sleep overshoot
    // closed-loop. Rollback: REX_DECODE_SERVO=0.
    {
        const char* ds = std::getenv("REX_DECODE_SERVO");
        m_decodeServoEnabled = !(ds && ds[0] == '0');
    }
    m_decodeServo.Reset();
    m_prevDecodeValid = false;
    m_width  = m_decoder->GetWidth();
    m_height = m_decoder->GetHeight();

    // Audio is optional and never fails the video: no audio track or a
    // broken audio device just means a silent wallpaper.
    auto audio = std::make_shared<AudioPlayer>();
    shared::Result<void> audioResult = audio->Open(path);
    if (audioResult.IsError()) {
        REX_LOG_WARN("VideoPlaybackController",
                     "audio disabled: {}", audioResult.ToString());
    } else {
        std::lock_guard lock(m_decodeMutex);
        std::lock_guard plock(m_decoderMutex);
        m_audio = std::move(audio);
    }

    m_position.store(0.0, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    m_framesDecoded.store(0, std::memory_order_relaxed);
    m_framesConsumed.store(0, std::memory_order_relaxed);
    m_framesDropped.store(0, std::memory_order_relaxed);
    // Fresh stream: drop any unpublished frames from the previous file and
    // disarm the burst gate — the playhead starts WITH the content, so pace
    // 1x until the first consume (see DecoderThread pacing).
    m_scheduler.Clear();

    // ── Prewarm (items 5+6): decode + publish the first frame HERE ─────
    // Two wins: the surface pool is built synchronously inside Open()
    // (resolution is known from the real frame — no lazy mid-playback
    // rebuild), and Play() can present instantly without waiting a full
    // decoder interval for frame #1.
    if (m_prewarmEnabled) {
        VideoFrame warm;
        {
            std::lock_guard lock(m_decodeMutex);
            if (m_decoder->DecodeFrame(warm)) {
                m_scheduler.Push(warm);
                m_decoderFrameCount = 1;
                m_convertFrameCount = 1;
                m_framesDecoded.store(1, std::memory_order_relaxed);
                m_position.store(warm.pts, std::memory_order_release);
                m_lastPublishedPts = warm.pts;
                std::lock_guard lastLock(m_lastFrameMutex);
                m_lastFrame.slot       = warm.slot;
                m_lastFrame.fenceValue = warm.fenceValue;
                m_lastFrame.width      = warm.width;
                m_lastFrame.height     = warm.height;
                m_lastFrame.frameIndex = warm.frameIndex;
            } else {
                REX_LOG_WARN("VideoPlaybackController",
                             "prewarm decode failed; first frame will arrive "
                             "from the decode thread");
            }
        }
    }

    m_consumerFrameCount = 0;
    m_fpsStartTime = Clock::now();

    m_threadRunning.store(true, std::memory_order_release);
    m_thread = std::thread(&VideoPlaybackController::DecoderThread, this);
    return shared::Result<void>();
}

void VideoPlaybackController::Close() {
    std::lock_guard lock(m_threadMutex);
    m_threadRunning.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    // P1-9: abort a decode thread blocked in av_read_frame BEFORE joining.
    // Leaf snapshot only (no m_decodeMutex — the blocked thread holds it).
    if (auto decoder = SnapshotDecoder()) {
        decoder->RequestStop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard lastLock(m_lastFrameMutex);
        m_lastFrame = FrameSnapshot{};
    }
    m_fenceHandle = nullptr;
    // P1-8: snapshot under both mutexes (op, then leaf), then tear down
    // outside them. A concurrent Seek/GetSlotHandle holds either null or
    // a live object via its own snapshot — never a half-reset pointer.
    std::shared_ptr<AudioPlayer> audio;
    std::shared_ptr<FfmpegVideoDecoder> decoder;
    {
        std::lock_guard lock(m_decodeMutex);
        std::lock_guard plock(m_decoderMutex);
        audio = m_audio;
        decoder = m_decoder;
        m_audio.reset();
        // m_decoder is deliberately kept (closed): it owns the shared-texture
        // pool the next Open reuses. It dies with the controller.
    }
    if (audio) {
        audio->Close();
    }
    if (decoder) {
        decoder->Close();
    }
    // The scene is gone: its end must not be reported any more (the app
    // would change the wallpaper for a scene that is no longer playing).
    m_sceneEnd.Rearm();
    m_open.store(false, std::memory_order_release);
}

void VideoPlaybackController::Play() {
    if (!m_open.load(std::memory_order_acquire)) return;
    if (!m_playing.load(std::memory_order_acquire) &&
        NeedsRewindOnPlay(m_eosReached.load(std::memory_order_acquire),
                          m_position.load(std::memory_order_acquire),
                          m_duration)) {
        Seek(0.0);
    }
    m_paused.store(false, std::memory_order_release);
    m_playing.store(true, std::memory_order_release);
    // Resume starts clean: a pause (auto-pause behind fullscreen apps,
    // suspend, user pause) leaves the playhead and the target wherever
    // they froze, so sprinting on resume only manufactures newest-due
    // skips. Hold 1x until the first consume re-arms the burst gate;
    // buffered pre-pause frames stay consumable (unlike Clear).
    m_scheduler.DisarmPrime();
    {
        // P1-8: touch the worker only while excluding Close()'s reset.
        std::lock_guard lock(m_decodeMutex);
        if (m_audio) m_audio->Play();
    }
}

void VideoPlaybackController::Pause() {
    m_paused.store(true, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_decodeMutex);
        if (m_audio) m_audio->Pause();
    }
}

void VideoPlaybackController::Stop() {
    m_paused.store(false, std::memory_order_relaxed);
    m_playing.store(false, std::memory_order_release);
    // P1-6: Stop resets the whole A/V timeline. The decoder seek was
    // missing, so Stop -> Play resumed video mid-stream while audio
    // restarted at 0. Seek 0 + scheduler clear (P1-5: no stale frame);
    // the clear runs even when the seek fails.
    // Stop/Seek race: the position reset MUST sit inside the lock after
    // Seek+Clear (it used to run outside: a frame published between the
    // reset and the seek resurrected stale PTS — StopTimelineTest proved
    // position 2.03 after Stop under load). The generation bump drops any
    // frame already decoded from the pre-stop stream.
    // P1-8: snapshot under the lock — a concurrent Close() resets the
    // members, and the pre-fix check-outside/use-inside was a UAF.
    std::shared_ptr<AudioPlayer> audio;
    {
        std::lock_guard lock(m_decodeMutex);
        if (m_open.load(std::memory_order_acquire) && m_decoder) {
            m_decoder->Seek(0.0);
            m_scheduler.Clear();
            m_lastPublishedPts = 0.0;
            ++m_decodeGeneration;
            m_position.store(0.0, std::memory_order_release);
            std::lock_guard clk(m_contentMutex);  // P0-2: no 0-jump artifact
            m_contentMeter.Reset();
        }
        audio = m_audio;
    }
    // Preserve the old unconditional-zero observable: with no decoder
    // present nothing can publish concurrently, so this is race-free
    // (with a decoder, the decode thread is parked on m_playing=false
    // and re-checks it under the lock before publishing).
    m_position.store(0.0, std::memory_order_release);
    m_eosReached.store(false, std::memory_order_release);  // timeline reset ⇒ playable
    m_sceneEnd.Rearm();  // timeline reset => not ended
    if (audio) {
        audio->Stop();
    }
}

void VideoPlaybackController::SetLoop(bool loop) {
    m_looping.store(loop, std::memory_order_relaxed);
    std::lock_guard lock(m_decodeMutex);
    if (m_decoder) {
        m_decoder->SetLoop(loop);
    }
    if (m_audio) {
        m_audio->SetLoop(loop);
    }
}

void VideoPlaybackController::SetSpeed(float speed) {
    const float s = speed > 0.0f ? speed : 1.0f;
    m_speed.store(s, std::memory_order_relaxed);
    // P1-13: tempo reaches audio too (was video-only → desync by design).
    std::lock_guard lock(m_decodeMutex);
    if (m_audio) m_audio->SetSpeed(s);
}

void VideoPlaybackController::SetVolume(float volume) {
    std::lock_guard lock(m_decodeMutex);
    if (m_audio) {
        m_audio->SetVolume(volume);
    }
}

void VideoPlaybackController::SetAudioOutputMode(AudioOutputMode mode) {
    std::lock_guard lock(m_decodeMutex);
    if (m_audio) {
        // Takes effect on the next endpoint (re)open; AudioPlayer keeps the
        // request and renegotiates rather than tearing down a playing stream.
        m_audio->SetOutputMode(mode);
    }
}

void VideoPlaybackController::SetMute(bool mute) {
    std::lock_guard lock(m_decodeMutex);
    if (m_audio) {
        if (!mute) {
            // The audio decode is SUSPENDED while nothing can be heard, so
            // the audio's own position is stale by the muted duration. Re-sync
            // it to the video's position BEFORE the decode resumes, so the
            // sound comes back in sync with the picture.
            m_audio->Seek(m_position.load(std::memory_order_relaxed));
        }
        m_audio->SetMute(mute);
    }
}

bool VideoPlaybackController::IsMuted() const {
    std::lock_guard lock(m_decodeMutex);
    return m_audio ? m_audio->IsMuted() : false;
}

shared::Result<void> VideoPlaybackController::Seek(double seconds) {
    // P1-8: check AND use under one lock — the pre-fix null check outside
    // the lock raced Close()'s reset (UAF on the use inside).
    std::lock_guard lock(m_decodeMutex);
    auto decoder = m_decoder;
    auto audio = m_audio;
    if (!m_open.load(std::memory_order_acquire) || !decoder) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "controller not open");
    }
    if (!decoder->Seek(seconds)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "seek failed");
    }
    m_position.store(seconds, std::memory_order_release);
    m_eosReached.store(false, std::memory_order_release);  // any seek re-arms playback
    // A seek re-arms the scene-end latch too: the app's playlist change
    // re-seeks the controller (VideoManager's cache-hit path), and a seek
    // that still read as "the video ended" would run the change loop away
    // (change -> seek -> ENDED -> change -> seek -> ...).
    m_sceneEnd.Rearm();
    ++m_decodeGeneration;  // drop any frame decoded from the pre-seek stream
    if (audio) {
        audio->Seek(seconds);
    }
    // P1-5: drop the pre-seek frame so the renderer never presents it;
    // the first post-seek frame re-arms the discontinuity detector.
    m_scheduler.Clear();
    m_lastPublishedPts = seconds;
    {
        // P0-2: the PTS jump across the seek must not read as a rate spike.
        std::lock_guard clk(m_contentMutex);
        m_contentMeter.Reset();
    }
    return shared::Result<void>();
}

void VideoPlaybackController::SelectBestStream(int targetWidth, int targetHeight) {
    if (!m_open.load(std::memory_order_acquire)) return;
    // P1-8: snapshot under the lock (pre-fix: null check outside raced reset).
    std::lock_guard lock(m_decodeMutex);  // excludes DecodeFrame and Seek
    auto decoder = m_decoder;
    if (!decoder) return;
    decoder->SelectStreamByResolution(targetWidth, targetHeight);

    // The new stream may have different properties: refresh everything the
    // controller cached from the decoder (pacing, telemetry dimensions).
    m_duration  = decoder->GetDuration();
    m_frameRate = decoder->GetFrameRate();
    m_contentStart = decoder->GetStartTime();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    m_width  = decoder->GetWidth();
    m_height = decoder->GetHeight();
    // Audit 5.4 #13: the reserve marks were scaled to the OPEN-time rate;
    // an adaptive switch can change the source rate (e.g. 24 -> 60 fps),
    // so the policy must be re-derived here too.
    RefreshDecodeAheadPolicy(/*logOverrides=*/false);
}

void VideoPlaybackController::RefreshDecodeAheadPolicy(bool logOverrides) {
    // Scale the reserve marks to what the 8-slot pool can actually hold at
    // this source rate. A fixed 100 ms mark is unreachable at >=60 fps and
    // made the decoder thrash burst->park->burst (measured: schedDrops 204,
    // effFps 52.5).
    m_decodeAheadPolicy = CadenceController::PolicyFor(m_frameRate);

    // Feature flag / rollback switch. The refill burst ships ON by default
    // since 2026-10-17 (precondition fix e5bfdf0 landed; A/B in the
    // DecodeAheadPolicy docs — content meter 1.00x in every ON run).
    // REX_DECODE_AHEAD > 1 sets an explicit multiplier; "0"/"1" forces the
    // burst off (rollback); unset keeps the default.
    if (const char* flag = std::getenv("REX_DECODE_AHEAD")) {
        const double burst = std::atof(flag);
        if (burst > 1.0) {
            m_decodeAheadPolicy.maxBurst = burst;
            if (logOverrides) {
                REX_LOG_INFO(kModule,
                             "decode-ahead ENABLED (burst={:.1f}x, lowWater={:.0f}ms, "
                             "highWater={:.0f}ms) via REX_DECODE_AHEAD",
                             burst, m_decodeAheadPolicy.lowWaterMs,
                             m_decodeAheadPolicy.highWaterMs);
            }
        } else {
            m_decodeAheadPolicy.maxBurst = 1.0;
            if (logOverrides) {
                REX_LOG_INFO(kModule,
                             "decode-ahead DISABLED (burst=1.0) via REX_DECODE_AHEAD");
            }
        }
    }
    // Catch-up burst: raises the decode-thread throughput ceiling ONLY while
    // published content trails the wall clock (reserveMs < 0). Refill pacing
    // is untouched, so this cannot build the deep reserve behind the 2x
    // defect. Default on (2.0); set REX_CATCHUP_BURST to override ("1" = off).
    if (const char* cuFlag = std::getenv("REX_CATCHUP_BURST")) {
        const double cu = std::atof(cuFlag);
        m_decodeAheadPolicy.catchUpBurst = cu > 1.0 ? cu : 1.0;
        if (logOverrides) {
            REX_LOG_INFO(kModule, "catch-up burst override ({:.1f}x) via REX_CATCHUP_BURST",
                         m_decodeAheadPolicy.catchUpBurst);
        }
    }
}

// ─── A/V sync watchdog ────────────────────────────────────────────────────
//
// The audio is clocked by the WASAPI endpoint and the video by the engine's
// wall clock, and NOTHING tied them together: both positions existed and
// nothing compared them, so a slow drift between the two clocks (or a
// device change that moved the endpoint's clock) was invisible.
//
// Both positions LEAD their playhead by a fixed amount - the audio ring's
// depth and the video scheduler's slots - so the raw difference is not the
// error; only its CHANGE over time is. The watchdog therefore measures the
// drift and corrects it only when it grows beyond a threshold large enough
// that it cannot be a scheduling artefact, and it never corrects while
// nothing can be heard: the decode is suspended then, so the audio position
// is frozen BY DESIGN and would look like a huge drift.
void VideoPlaybackController::CheckAvSync() {
    std::shared_ptr<AudioPlayer> audio;
    {
        std::lock_guard lock(m_decodeMutex);
        audio = m_audio;
    }
    if (!audio) {
        return;
    }
    if (!audio->HasAudio()) {
        // No audio track in this media (or the stream never opened): the
        // audio position stays at 0 for the whole playback, so the watchdog
        // would measure the ENTIRE video position as "drift" and re-seek the
        // audio every few seconds. Each of those re-seeks also blocked the
        // render thread for ~170 ms (measured on a 4K 60 fps clip with no
        // audio track: a visible hitch every 2.8 s). There is nothing to
        // sync against, so the watchdog must not run at all.
        m_avSyncBaselineValid = false;
        return;
    }
    if (audio->IsSilentOutput()) {
        // Silent: the decode is suspended, so the audio position is frozen.
        // Forget the baseline so the next audible check starts clean.
        m_avSyncBaselineValid = false;
        return;
    }
    const double videoPos = m_position.load(std::memory_order_relaxed);
    const double audioPos = audio->GetPlayedPosition();
    if (!std::isfinite(videoPos) || !std::isfinite(audioPos)) {
        return;
    }
    const double drift = audioPos - videoPos;
    if (!m_avSyncBaselineValid) {
        m_avSyncBaseline = drift;
        m_avSyncBaselineValid = true;
        return;
    }
    const double change = drift - m_avSyncBaseline;
    m_avSyncDriftMs.store(change * 1000.0, std::memory_order_relaxed);
    if (!AvSyncNeedsCorrection(change * 1000.0, kAvSyncCorrectionMs)) {
        // Telemetry: a sync error is silent by nature, so the drift has to be
        // visible. This runs every kAvSyncCheckFrames (~4 s), so it is a
        // debug-level line; the correction below is the warning.
        REX_LOG_DEBUG(kModule,
                      "A/V drift {:+.1f} ms ({} corrections so far)",
                      change * 1000.0,
                      m_avSyncCorrections.load(std::memory_order_relaxed));
        return;
    }
    // The clocks have really parted: put the audio back on the video's
    // position. The seek is request-based (the audio thread performs it), so
    // this is safe from the render thread.
    REX_LOG_WARN(kModule,
                 "A/V drift {:.0f} ms: re-seeking the audio to {:.3f} s "
                 "(video {:.3f} s, audio {:.3f} s) - correction #{}",
                 change * 1000.0, videoPos, videoPos, audioPos,
                 m_avSyncCorrections.load(std::memory_order_relaxed) + 1);
    audio->Seek(videoPos);
    m_avSyncBaseline = drift;
    m_avSyncCorrections.fetch_add(1, std::memory_order_relaxed);
}

const VideoFrame* VideoPlaybackController::Tick(double targetPts) {
    // Loop-relative target (Phase 2): the decoder wraps PTS every
    // duration while the wall marches on — an unwrapped target starves
    // the render loop past the first loop and explodes the drop counter.
    const double wrapped = WrapTarget(
        targetPts, m_duration, m_looping.load(std::memory_order_relaxed));
    // P0-4: publish the loop-relative wall target for the decode thread's
    // lead computation (same PTS epoch as published frames, so loop wraps
    // stay comparable; a one-iteration transient either bursts or parks,
    // both safe directions).
    m_lastTargetPts.store(wrapped, std::memory_order_relaxed);
    // Backlog recovery: the published content is more than a second behind
    // the wall clock (the renderer stalled, the ring filled, the decoder
    // blocked). Let the scheduler jump the content forward so the ring
    // drains - otherwise the backlog is an equilibrium and took ~90 s to
    // unwind in the measured case (reserve -3831 ms, 2-3 fps).
    const bool inBacklog =
        m_reserveMs.load(std::memory_order_relaxed) < -kBacklogRecoveryMs;
    const VideoFrame* frame = m_scheduler.Tick(wrapped, inBacklog);
    if (frame) {
        m_consumerFrameCount++;
        m_framesConsumed.fetch_add(1, std::memory_order_relaxed);
        m_framesDropped.store(m_scheduler.GetDroppedCount(),
                              std::memory_order_relaxed);
        {
            // P0-2: feed the content clock from what was actually presented.
            std::lock_guard clk(m_contentMutex);
            m_contentMeter.OnPresented(frame->pts, m_duration, NowSec());
        }
        // Scene end (presented side, secondary): the frame on screen
        // reached the end of the content. The decode thread sets the same
        // latch from the PUBLISHED PTS - that is the side that always
        // sees the end, because the scheduler clears the tail frames when
        // the decoder wraps. Kept as the belt to that braces: a frame that
        // reaches the end on screen is the end of the content, whatever
        // the decoder did.
        (void)m_sceneEnd.OnFrame(frame->pts, m_contentStart, m_duration,
                                 m_frameRate);
        // A/V sync watchdog (see CheckAvSync).
        if (++m_avSyncTicks >= kAvSyncCheckFrames) {
            m_avSyncTicks = 0;
            CheckAvSync();
        }
    } else if (m_playing.load(std::memory_order_relaxed) &&
               m_scheduler.PendingCount() == 0) {
        // Phase D underrun: playing, but the reserve held nothing at all, so
        // the render loop must repeat the last frame. Counted separately from
        // ordinary pacing repeats so reserve effectiveness is measurable.
        m_reserveUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    return frame;
}

HANDLE VideoPlaybackController::GetSlotHandle(UINT slot) const {
    // P1-8 + P1-9: leaf snapshot only — never blocks behind a decode in
    // progress (the render thread's busiest call path).
    auto decoder = SnapshotDecoder();
    return decoder ? decoder->GetSlotHandle(slot) : nullptr;
}

UINT64 VideoPlaybackController::GetSlotFence(UINT slot) const {
    auto decoder = SnapshotDecoder();
    return decoder ? decoder->GetSlotFence(slot) : 0;
}

VideoPlaybackController::FrameSnapshot VideoPlaybackController::GetLastFrame() const {
    std::lock_guard lock(m_lastFrameMutex);
    return m_lastFrame;
}

VideoPlaybackTelemetry VideoPlaybackController::GetTelemetry() const {
    VideoPlaybackTelemetry t;
    t.framesDecoded = m_framesDecoded.load(std::memory_order_relaxed);
    t.framesConsumed = m_framesConsumed.load(std::memory_order_relaxed);
    t.framesDropped = m_framesDropped.load(std::memory_order_relaxed);
    const double secs = std::chrono::duration<double>(Clock::now() - m_fpsStartTime).count();
    t.decoderFps = secs > 0.0 ? static_cast<double>(m_decoderFrameCount) / secs : 0.0;
    t.renderFps = secs > 0.0 ? static_cast<double>(m_consumerFrameCount) / secs : 0.0;
    t.avSyncDriftMs = m_avSyncDriftMs.load(std::memory_order_relaxed);
    t.avSyncCorrections = m_avSyncCorrections.load(std::memory_order_relaxed);
    t.sourceFps = m_frameRate;
    t.convertFps = secs > 0.0 ? static_cast<double>(m_convertFrameCount) / secs : 0.0;
    t.schedulerDrops = m_framesDropped.load(std::memory_order_relaxed);
    // Fold the cumulative drop counter into a RATE, exactly like presentFps.
    // The cumulative value keeps climbing for the life of the process, so a
    // log line reading "schedDrops=694" looks like an escalating failure even
    // when the scheduler has dropped nothing for minutes (measured 2026-11:
    // the counter froze at 103 while effFps sat at 59).
    {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - m_dropRateTime).count();
        // Window must be LONGER than the telemetry sampler's interval (5 s),
        // otherwise a burst that begins and ends between two log lines is
        // averaged away and reported as 0.0/s — exactly the blindness this
        // metric exists to remove (measured 2026-11: 39 and 40 drop bursts
        // were only visible because they happened to span a fold).
        constexpr double kDropWindowSec = 10.0;
        if (dt >= kDropWindowSec) {
            if (m_dropRateTime.time_since_epoch().count() != 0) {
                const double delta = static_cast<double>(t.schedulerDrops) -
                                     static_cast<double>(m_dropRateBase);
                m_dropRatePerSec = delta / dt;
                // Biggest single jump seen inside the window, so a spike that
                // the average smooths away is still visible.
                if (delta > m_dropRatePeak) m_dropRatePeak = delta;
            }
            m_dropRateBase = t.schedulerDrops;
            m_dropRateTime = now;
        } else if (m_dropRateTime.time_since_epoch().count() != 0) {
            // Inside the window: track the peak without resetting the base.
            const double delta = static_cast<double>(t.schedulerDrops) -
                                 static_cast<double>(m_dropRateBase);
            if (delta > m_dropRatePeak) m_dropRatePeak = delta;
        }
        t.schedulerDropsPerSec = m_dropRatePerSec;
        t.schedulerDropsPeak = m_dropRatePeak;
    }
    t.loopWraps = m_loopWraps.load(std::memory_order_relaxed);
    t.sceneEnded = m_sceneEnd.Ended();
    t.queueDepth = m_scheduler.PendingCount();
    t.pacingMs = m_frameIntervalMs / m_speed.load(std::memory_order_relaxed);
    // P0-2: fold the presented-PTS content clock (10 s window). Reports the
    // LAST measured rate inside a window - never a partial-window guess.
    {
        std::lock_guard clk(m_contentMutex);
        t.contentRate = m_contentMeter.Fold(NowSec());
    }
    // Phase D: report the MEASURED reserve and the actual decode-ahead rate,
    // not the nominal 200 ms / 2x targets. The pool caps the achievable
    // depth, and reporting the target would overstate it at high source rates.
    t.reserveMs = m_reserveMs.load(std::memory_order_relaxed);
    t.decodeServoCorrMs = m_decodeServoCorrMs.load(std::memory_order_relaxed);
    t.decodeAheadRate = (m_frameRate > 0.0 && t.decoderFps > 0.0)
                            ? t.decoderFps / m_frameRate : 1.0;
    t.reserveUnderruns = m_reserveUnderruns.load(std::memory_order_relaxed);
    // Content rate (Phase 3): decode throughput over source rate.
    // 1.0 = realtime; below means decode-bound (repeats fill the gap).
    t.playbackSpeed = m_frameRate > 0.0
        ? (secs > 0.0 ? static_cast<double>(m_decoderFrameCount) / secs : 0.0)
          / m_frameRate
        : 1.0;
    t.ringSlotCount = m_scheduler.GetNumSlots();
    t.ringBytes = static_cast<uint64_t>(m_width) * static_cast<uint64_t>(m_height) *
                  4 * m_scheduler.GetNumSlots();
    // P1-8: snapshot the decoder (pre-fix unlocked read raced Close()).
    auto decoder = SnapshotDecoder();
    if (decoder) {
        const DecoderDiagnostics d = decoder->GetDiagnostics();
        t.hardwareAccelerated = d.hardwareAccelerated;
        t.gpuToCpuCopies = d.gpuToCpuCopies;
        t.cpuToGpuCopies = d.cpuToGpuCopies;
        t.uploadLatencyMs = d.lastDecodeMs;
        t.decodeMsP50 = d.decodeMsP50;
        t.decodeMsP95 = d.decodeMsP95;
        t.decodeMsP99 = d.decodeMsP99;
        t.vpMsP50     = d.vpMsP50;
        t.vpMsP95     = d.vpMsP95;
        t.vpMsP99     = d.vpMsP99;
        t.recoveryCount = d.recoveryCount;
        t.sendEagainStalls = d.sendEagainStalls;
        t.decodePathUnusable = d.pathUnusable;
        t.hwDecoderProfiles = d.hwDecoderProfiles;
        t.deviceResets = d.deviceResets;
    }
    return t;
}

void VideoPlaybackController::DecoderThread() {
    // Diagnostic thread role (ETW/debugger-visible, behavior-neutral).
    rex::shared::SetCurrentThreadName(L"REX-Decode");
    while (m_threadRunning.load(std::memory_order_acquire)) {
        if (!m_playing.load(std::memory_order_acquire) ||
            m_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // NOTE (frozen 2026-10-15): a reserve-aware pre-check that skipped
        // decoding when reserve >= high water lived here and was REMOVED. It
        // deadlocked whenever consumption stalled for any reason (stop/restart,
        // slow playhead in tests): a full ring of future frames kept the skip
        // firing, so no new frame was ever published and nothing ever became
        // due. Decode pacing below already handles reserve depth without
        // refusing to decode; the ring-full backstop further down covers the
        // pathological case.

        VideoFrame frame;
        bool ok = false;
        bool looping = true;
        const auto t0 = Clock::now();
        {
            std::lock_guard lock(m_decodeMutex);
            // Re-check under the lock: Stop()/Pause() may have landed
            // after the loop-top check — publishing then would resurrect
            // stale PTS past the reset (StopTimelineTest proved it live:
            // position 2.03 after Stop under load).
            if (!m_playing.load(std::memory_order_acquire) ||
                m_paused.load(std::memory_order_acquire)) {
                continue;
            }
            auto decoder = m_decoder;
            const uint64_t gen = m_decodeGeneration;
            ok = decoder && decoder->DecodeFrame(frame);
            // The video elementary stream ran out while decoding this frame
            // (the decoder rewinded internally when looping). Latch the scene
            // end from that fact: the container duration can sit 1-2 s past
            // the last video frame (audio tail), so the PTS comparison alone
            // never reports the end for such files.
            const bool reachedStreamEnd = decoder && decoder->ReachedEndOfStream();
            // P1-8: read the flag from the snapshot, not the member —
            // Close() may reset it the moment the lock releases.
            looping = decoder && decoder->GetLoop();
            if (reachedStreamEnd) {
                m_sceneEnd.MarkEnded();
            }
            if (ok) {
                // Drop frames decoded from a pre-Seek/Stop stream: the
                // generation moved while DecodeFrame was inside.
                if (gen != m_decodeGeneration) {
                    ok = false;
                    looping = true;
                } else {
                    // P1-5: a backward PTS jump means the decoder seeked
                    // internally (loop wrap) — drop the pre-seek frame.
                    // Clear is idempotent, so an explicit-Seek double-clear
                    // (keyframe lands before the target) is harmless.
                    if (IsBackwardJump(m_lastPublishedPts, frame.pts)) {
                        REX_LOG_WARN(kModule,
                                     "backward-jump: lastPts={:.3f} "
                                     "framePts={:.3f} -> scheduler Clear",
                                     m_lastPublishedPts, frame.pts);
                        m_scheduler.Clear();
                        {
                            // P0-2: the content clock's watermark belongs to
                            // the old epoch - reseed it with the new stream,
                            // or the wrap reads as a rate excursion.
                            std::lock_guard clk(m_contentMutex);
                            m_contentMeter.Reset();
                        }
                        m_loopWraps.fetch_add(1, std::memory_order_relaxed);
                    }
                    m_lastPublishedPts = frame.pts;
                    // Publish under the same lock: Stop()/Seek() reset
                    // position under this lock, so push + PTS store are
                    // atomic against their reset (previously the store ran
                    // outside and won the race).
                    m_scheduler.Push(frame);
                    m_decoderFrameCount++;
                    m_convertFrameCount++;
                    m_framesDecoded.fetch_add(1, std::memory_order_relaxed);
                    m_position.store(frame.pts, std::memory_order_release);
                    // Scene end (published side): the decoder reached the end
                    // of the content. This is the side that SEES the end:
                    // when the decoder wraps (loop) or hits EOF, it has
                    // already published the tail frames, and the scheduler
                    // then clears them - so the PRESENTED PTS never
                    // reaches the duration and a presented-side check
                    // never fires (measured: a 54 s clip looped without
                    // ever reporting its end).
                    //
                    // A backward PTS jump (a seek, a loop wrap) can never
                    // satisfy the comparison: the sum only grows with
                    // forward progress.
                    if (m_sceneEnd.OnFrame(frame.pts, m_contentStart,
                                           m_duration, m_frameRate)) {
                        REX_LOG_INFO(kModule,
                                     "scene ended: pts={:.3f} duration={:.3f}",
                                     frame.pts, m_duration);
                    }
                    // Stage sample stamp (diagnostic): scheduler depth at the
                    // moment this frame was published.
                    if (decoder) {
                        decoder->StampStageQueueDepth(
                            static_cast<int64_t>(m_scheduler.PendingCount()));
                    }
                    {
                        std::lock_guard lastLock(m_lastFrameMutex);
                        m_lastFrame.slot = frame.slot;
                        m_lastFrame.fenceValue = frame.fenceValue;
                        m_lastFrame.width = frame.width;
                        m_lastFrame.height = frame.height;
                        m_lastFrame.frameIndex = frame.frameIndex;
                    }
                }
            }
        }
        if (!ok) {
            if (looping) {
                // P1-10: DecodeFrame failed with loop on (seek refused, empty
                // file, persistent error) — back off instead of busy-spinning
                // at 100% CPU. Normal EOS looping never lands here: the
                // decoder seeks internally and returns a frame.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;  // decoder already looped internally
            }
            m_playing.store(false, std::memory_order_release);  // end of stream
            // Latch EOS (audit 2026-10-17, §5.4 #5): the published position
            // alone cannot mark "finished" (last PTS is one frame short of
            // the duration), so Play() would refuse to restart a non-looping
            // video that played to completion. Seek/Stop/Open clear this.
            m_eosReached.store(true, std::memory_order_release);
            continue;
        }

        // (Publish moved inside the decode lock above: push + PTS store
        // must be atomic against Stop()/Seek() resets under the same lock.)

        // Decode pacing (Phase 2 revised): throttle to the source frame
        // rate (speed-scaled) so the 8-slot ring always holds frames near
        // the wall target. Paced decode keeps the lead at ~1 frame; a
        // decode shortfall (AV1 33 < 60) still costs frames, and Phase 3
        // re-presents at wall cadence so time stays 1x. The queue-depth
        // park below covers post-hitch bursts. (Full wall-rate scaling
        // of the timeline lands in Phase 4.)
        //
        // The wait MUST be PreciseSleepUntil, not sleep_for: plain sleeps
        // quantize to the 15.6 ms Windows quantum, so a 16.7 ms pace wakes
        // after ~31 ms and EVERY 60 fps source decodes at ~32 fps (proven
        // live 2026-10-07: pacingMs 16.67 + decodeMsP50 0.5 ms yet
        // decoderFps stuck at 33.0). The high-res waitable timer is
        // immune to the quantum.
        const double sourceIntervalMs =
            m_frameIntervalMs / m_speed.load(std::memory_order_relaxed);

        // Phase D: pace by RESERVE DEPTH rather than a flat source-rate
        // interval. Below the low-water mark the decoder bursts (when enabled)
        // to rebuild the reserve; in band, source rate; above the high-water
        // mark it parks so the playhead — not an ever-growing lead — bounds
        // the ring (P0-4). Behind the playhead it bursts to catch up and
        // never parks (a starved playhead must not be throttled: that was
        // the reverted 0.6x regression).
        //
        // The lead is recomputed from live atomics every iteration —
        // stateless, so stop/pause/seek transients self-correct and cannot
        // deadlock the way the reverted latched skip-pre-check did.
        const double leadMs =
            (m_position.load(std::memory_order_relaxed) -
             m_lastTargetPts.load(std::memory_order_relaxed)) *
            1000.0;
        m_reserveMs.store(!(leadMs >= 0.0) && !(leadMs < 0.0) ? 0.0 : leadMs,
                          std::memory_order_relaxed);
        const double intervalMs = m_scheduler.IsPrimed()
            ? CadenceController::DecodeIntervalMs(
                  sourceIntervalMs,
                  m_reserveMs.load(std::memory_order_relaxed),
                  m_decodeAheadPolicy)
            // Unprimed (nothing consumed since Open/Seek/Clear): the playhead
            // starts WITH the content — there is no backlog to catch up to,
            // so pace at the source rate. Bursting here only manufactures
            // newest-due skips (the load/wrap startup storm). Once the first
            // frame is consumed the normal reserve policy takes over.
            : sourceIntervalMs;

        // Closed-loop decode pacing: the open-loop sleep overshoots by a
        // steady ~+0.5 ms per iteration (measured: convFps 3-4% under source
        // at 60 fps), which no fixed interval can recover — the reserve pins
        // negative and catch-up burst cycles forever. Feed the achieved
        // iteration interval back through the servo and trim the deadline.
        // Stalls/pauses surface as one clamped correction (the servo's
        // conditional integration refuses to bank them).
        double servoCorrMs = 0.0;
        if (m_decodeServoEnabled && intervalMs > 0.0) {
            if (m_prevDecodeValid) {
                const double achievedMs =
                    std::chrono::duration<double, std::milli>(
                        t0 - m_prevDecodeStart)
                        .count();
                servoCorrMs =
                    m_decodeServo.Step(achievedMs - intervalMs, intervalMs);
            }
            m_prevDecodeStart = t0;
            m_prevDecodeValid = true;
        } else {
            m_prevDecodeValid = false;
        }
        m_decodeServoCorrMs.store(servoCorrMs, std::memory_order_relaxed);

        rex::shared::PreciseSleepUntil(
            t0 + std::chrono::duration<double, std::milli>(intervalMs +
                                                           servoCorrMs));
        // Ring-full backstop. This is now a SAFETY net, not the primary
        // throttle — the reserve-aware check at the top of the loop handles the
        // normal "reserve is healthy" case. It only catches the pathological
        // one: we decoded into an already-full ring, which means the playhead
        // is not consuming and the next write would be an overwrite of
        // unread content.
        //
        // High-res wait: plain sleep_for(2 ms) quantizes to ~15 ms on the
        // default quantum and throttles steady decode to ~66 fps (proven
        // live at 2x demand) — the same quantum bug as pacing.
        if (m_scheduler.PendingCount() >= FrameScheduler::kNumSlots) {
            rex::shared::PreciseSleepUntil(
                Clock::now() + std::chrono::milliseconds(2));
        }
        // Per-frame stage CSV (diagnostic): one dump per 5 s of decode-loop
        // time, only when REX_PERFRAME_CSV=1 (DumpDecodeStages re-checks the
        // flag and no-ops otherwise). Ring snapshot tolerates the decode
        // thread pushing concurrently (same discipline as P13 rings).
        {
            const auto nowDump = Clock::now();
            if (nowDump - m_lastStageDump > std::chrono::seconds(5)) {
                m_lastStageDump = nowDump;
                // SnapshotDecoder: same P1-8 discipline as every other
                // decoder access (Close() may reset m_decoder concurrently).
                auto dec = SnapshotDecoder();
                if (dec) {
                    const size_t rows = dec->DumpDecodeStages();
                    if (rows > 0) {
                        REX_LOG_INFO(kModule,
                                     "per-frame stages dumped ({} rows)",
                                     rows);
                    }
                }
            }
        }
    }
}

} // namespace rex::wallpaper
