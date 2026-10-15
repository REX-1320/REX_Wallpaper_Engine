#include "wallpaper/playback/VideoPlaybackController.hpp"

#include <shared/Logger.hpp>
#include <shared/PreciseSleep.hpp>

#include <chrono>
#include <cstdlib>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "VideoPlaybackController";
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

    auto decoder = std::make_shared<FfmpegVideoDecoder>();
    shared::Result<void> result = decoder->Open(path, useHardware, context11, fenceHandle);
    if (result.IsError()) {
        return result;
    }

    {
        // Publish under both mutexes (op, then leaf): a concurrent
        // Seek/Close snapshots either null or the fully opened decoder.
        std::lock_guard lock(m_decodeMutex);
        std::lock_guard plock(m_decoderMutex);
        m_decoder = std::move(decoder);
    }
    m_fenceHandle = fenceHandle;
    m_duration  = m_decoder->GetDuration();
    m_frameRate = m_decoder->GetFrameRate();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    {
        // P0-2: a reopen must not inherit the previous file's content clock.
        std::lock_guard clk(m_contentMutex);
        m_contentMeter.Reset();
    }
    // Scale the reserve marks to what the 8-slot pool can actually hold at this
    // source rate. A fixed 100 ms mark is unreachable at >=60 fps and made the
    // decoder thrash burst->park->burst (measured: schedDrops 204, effFps 52.5).
    m_decodeAheadPolicy = CadenceController::PolicyFor(m_frameRate);

    // Feature flag / rollback switch. Decode-ahead is OFF by default: it
    // measurably helps low-fps GPU-stalled content but a deep ring makes the
    // scheduler count still-buffered frames as drops. Set REX_DECODE_AHEAD to
    // an explicit burst multiplier (e.g. "2") to enable, "0"/unset to disable.
    // Measured A/B belongs in wall_research.md; nothing should re-enable this
    // without that comparison.
    if (const char* flag = std::getenv("REX_DECODE_AHEAD")) {
        const double burst = std::atof(flag);
        if (burst > 1.0) {
            m_decodeAheadPolicy.maxBurst = burst;
            REX_LOG_INFO(kModule,
                         "decode-ahead ENABLED (burst={:.1f}x, lowWater={:.0f}ms, "
                         "highWater={:.0f}ms) via REX_DECODE_AHEAD",
                         burst, m_decodeAheadPolicy.lowWaterMs,
                         m_decodeAheadPolicy.highWaterMs);
        }
    }
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
        m_decoder.reset();
    }
    if (audio) {
        audio->Close();
    }
    if (decoder) {
        decoder->Close();
    }
    m_open.store(false, std::memory_order_release);
}

void VideoPlaybackController::Play() {
    if (!m_open.load(std::memory_order_acquire)) return;
    if (!m_playing.load(std::memory_order_acquire) &&
        m_position.load(std::memory_order_acquire) >= m_duration - 0.001) {
        Seek(0.0);
    }
    m_paused.store(false, std::memory_order_release);
    m_playing.store(true, std::memory_order_release);
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
            m_scheduler.ResetPresentationClock();
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
    ++m_decodeGeneration;  // drop any frame decoded from the pre-seek stream
    if (audio) {
        audio->Seek(seconds);
    }
    // P1-5: drop the pre-seek frame so the renderer never presents it;
    // the first post-seek frame re-arms the discontinuity detector.
    m_scheduler.Clear();
    m_scheduler.ResetPresentationClock();
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
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    m_width  = decoder->GetWidth();
    m_height = decoder->GetHeight();
}

const VideoFrame* VideoPlaybackController::Tick(double targetPts) {
    // Loop-relative target (Phase 2): the decoder wraps PTS every
    // duration while the wall marches on — an unwrapped target starves
    // the render loop past the first loop and explodes the drop counter.
    const double wrapped = WrapTarget(
        targetPts, m_duration, m_looping.load(std::memory_order_relaxed));
    const VideoFrame* frame = m_scheduler.Tick(wrapped);
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
        t.hwDecoderProfiles = d.hwDecoderProfiles;
        t.deviceResets = d.deviceResets;
    }
    return t;
}

void VideoPlaybackController::DecoderThread() {
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
            // P1-8: read the flag from the snapshot, not the member —
            // Close() may reset it the moment the lock releases.
            looping = decoder && decoder->GetLoop();
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
                        m_scheduler.Clear();
                        m_scheduler.ResetPresentationClock();
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
        // to rebuild the reserve; at or above it, source rate.
        const double intervalMs = CadenceController::DecodeIntervalMs(
            sourceIntervalMs, m_reserveMs.load(std::memory_order_relaxed),
            m_decodeAheadPolicy);

        rex::shared::PreciseSleepUntil(
            t0 + std::chrono::duration<double, std::milli>(intervalMs));
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
    }
}

} // namespace rex::wallpaper
