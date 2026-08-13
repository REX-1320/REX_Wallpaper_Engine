#include "wallpaper/playback/VideoPlaybackController.hpp"

#include <shared/Logger.hpp>

#include <chrono>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "VideoPlaybackController";
using ms = std::chrono::duration<double, std::milli>;
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

    auto decoder = std::make_unique<FfmpegVideoDecoder>();
    shared::Result<void> result = decoder->Open(path, useHardware, context11, fenceHandle);
    if (result.IsError()) {
        return result;
    }

    m_decoder = std::move(decoder);
    m_fenceHandle = fenceHandle;
    m_duration  = m_decoder->GetDuration();
    m_frameRate = m_decoder->GetFrameRate();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    m_width  = m_decoder->GetWidth();
    m_height = m_decoder->GetHeight();

    // Audio is optional and never fails the video: no audio track or a
    // broken audio device just means a silent wallpaper.
    m_audio = std::make_unique<AudioPlayer>();
    shared::Result<void> audioResult = m_audio->Open(path);
    if (audioResult.IsError()) {
        REX_LOG_WARN("VideoPlaybackController",
                     "audio disabled: {}", audioResult.ToString());
        m_audio.reset();
    }

    m_position.store(0.0, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    m_framesDecoded.store(0, std::memory_order_relaxed);
    m_framesConsumed.store(0, std::memory_order_relaxed);
    m_framesDropped.store(0, std::memory_order_relaxed);
    m_decoderFrameCount = 0;
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
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard lastLock(m_lastFrameMutex);
        m_lastFrame = FrameSnapshot{};
    }
    m_fenceHandle = nullptr;
    if (m_audio) {
        m_audio->Close();
        m_audio.reset();
    }
    if (m_decoder) {
        m_decoder->Close();
    }
    m_decoder.reset();
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
    if (m_audio) {
        m_audio->Play();
    }
}

void VideoPlaybackController::Pause() {
    m_paused.store(true, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    if (m_audio) {
        m_audio->Pause();
    }
}

void VideoPlaybackController::Stop() {
    m_paused.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    m_position.store(0.0, std::memory_order_release);
    if (m_audio) {
        m_audio->Stop();
    }
}

void VideoPlaybackController::SetLoop(bool loop) {
    m_looping.store(loop, std::memory_order_relaxed);
    if (m_decoder) {
        m_decoder->SetLoop(loop);
    }
    if (m_audio) {
        m_audio->SetLoop(loop);
    }
}

void VideoPlaybackController::SetSpeed(float speed) {
    m_speed.store(speed > 0.0f ? speed : 1.0f, std::memory_order_relaxed);
}

void VideoPlaybackController::SetVolume(float volume) {
    if (m_audio) {
        m_audio->SetVolume(volume);
    }
}

void VideoPlaybackController::SetMute(bool mute) {
    if (m_audio) {
        m_audio->SetMute(mute);
    }
}

shared::Result<void> VideoPlaybackController::Seek(double seconds) {
    if (!m_open.load(std::memory_order_acquire) || !m_decoder) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "controller not open");
    }
    std::lock_guard lock(m_decodeMutex);
    if (!m_decoder->Seek(seconds)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "seek failed");
    }
    m_position.store(seconds, std::memory_order_release);
    if (m_audio) {
        m_audio->Seek(seconds);
    }
    return shared::Result<void>();
}

void VideoPlaybackController::SelectBestStream(int targetWidth, int targetHeight) {
    if (!m_open.load(std::memory_order_acquire) || !m_decoder) return;
    std::lock_guard lock(m_decodeMutex);  // excludes DecodeFrame and Seek
    m_decoder->SelectStreamByResolution(targetWidth, targetHeight);

    // The new stream may have different properties: refresh everything the
    // controller cached from the decoder (pacing, telemetry dimensions).
    m_duration  = m_decoder->GetDuration();
    m_frameRate = m_decoder->GetFrameRate();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    m_width  = m_decoder->GetWidth();
    m_height = m_decoder->GetHeight();
}

const VideoFrame* VideoPlaybackController::Tick(double targetPts) {
    const VideoFrame* frame = m_scheduler.Tick(targetPts);
    if (frame) {
        m_consumerFrameCount++;
        m_framesConsumed.fetch_add(1, std::memory_order_relaxed);
        const uint64_t decoded = m_framesDecoded.load(std::memory_order_relaxed);
        const uint64_t consumed = m_framesConsumed.load(std::memory_order_relaxed);
        m_framesDropped.store(decoded > consumed ? decoded - consumed - 1 : 0,
                              std::memory_order_relaxed);
    }
    return frame;
}

HANDLE VideoPlaybackController::GetSlotHandle(UINT slot) const {
    return m_decoder ? m_decoder->GetSlotHandle(slot) : nullptr;
}

UINT64 VideoPlaybackController::GetSlotFence(UINT slot) const {
    return m_decoder ? m_decoder->GetSlotFence(slot) : 0;
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
    t.ringSlotCount = m_scheduler.GetNumSlots();
    t.ringBytes = static_cast<uint64_t>(m_width) * static_cast<uint64_t>(m_height) *
                  4 * m_scheduler.GetNumSlots();
    if (m_decoder) {
        const DecoderDiagnostics d = m_decoder->GetDiagnostics();
        t.hardwareAccelerated = d.hardwareAccelerated;
        t.gpuToCpuCopies = d.gpuToCpuCopies;
        t.cpuToGpuCopies = d.cpuToGpuCopies;
        t.uploadLatencyMs = d.lastDecodeMs;
        t.recoveryCount = d.recoveryCount;
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

        const auto t0 = Clock::now();
        VideoFrame frame;
        bool ok = false;
        {
            std::lock_guard lock(m_decodeMutex);
            ok = m_decoder && m_decoder->DecodeFrame(frame);
        }
        if (!ok) {
            if (m_decoder && m_decoder->GetLoop()) {
                continue;  // decoder already looped internally
            }
            m_playing.store(false, std::memory_order_release);  // end of stream
            continue;
        }

        m_scheduler.Push(frame);
        m_decoderFrameCount++;
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

        // Pace decode to the source frame rate (speed-scaled).
        const double elapsed = ms(Clock::now() - t0).count();
        const double interval =
            m_frameIntervalMs / m_speed.load(std::memory_order_relaxed);
        const double remaining = interval - elapsed;
        if (remaining > 0.5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(remaining)));
        }
    }
}

} // namespace rex::wallpaper
