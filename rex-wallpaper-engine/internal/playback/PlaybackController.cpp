#include "PlaybackController.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper {

PlaybackController::PlaybackController()
    : m_decoder(std::make_unique<MediaFoundationDecoder>()) {
    m_fpsStartTime = Clock::now();
}

PlaybackController::~PlaybackController() {
    Close();
}

shared::Result<void> PlaybackController::Open(std::string_view path) {
    std::lock_guard lock(m_threadMutex);

    if (m_decoder->IsOpen()) {
        m_threadRunning = false;
        if (m_decoderThread.joinable()) {
            m_decoderThread.join();
        }
        m_decoder->Close();
    }

    auto result = m_decoder->Open(path);
    if (result.IsError()) {
        return result;
    }

    m_width        = m_decoder->GetWidth();
    m_height       = m_decoder->GetHeight();
    m_frameRate    = m_decoder->GetFrameRate();
    m_frameInterval = (m_frameRate > 0.0) ? (1.0 / m_frameRate) : (1.0 / 30.0);
    m_duration     = m_decoder->GetDuration();
    m_writeCount.store(0, std::memory_order_relaxed);
    m_lastReadCount = 0;
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_open.store(true, std::memory_order_release);

    int frameBytes = m_width * m_height * 4;
    for (int i = 0; i < kNumRingSlots; ++i) {
        m_ring[i].pixels.reserve(static_cast<size_t>(frameBytes));
        m_ring[i].width      = m_width;
        m_ring[i].height     = m_height;
        m_ring[i].rowPitch   = m_width * 4;
        m_ring[i].format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    REX_LOG_INFO("Playback", "Opened video: {}x{} @ {:.2f} fps, {:.2f}s, {}B/frame",
                 m_width, m_height, m_frameRate, m_duration, frameBytes);
    return shared::Result<void>();
}

void PlaybackController::Close() {
    std::lock_guard lock(m_threadMutex);

    m_playing.store(false, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_open.store(false, std::memory_order_relaxed);
    m_threadRunning = false;

    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    if (m_decoder) {
        m_decoder->Close();
    }

    m_currentPosition.store(0.0, std::memory_order_relaxed);
    REX_LOG_INFO("Playback", "Closed");
}

void PlaybackController::Play() {
    if (!m_open.load()) return;

    m_playing.store(true, std::memory_order_release);
    m_paused.store(false, std::memory_order_relaxed);
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_writeCount.store(0, std::memory_order_relaxed);
    m_lastReadCount = 0;

    // Reset telemetry for new playback session
    m_telemetry = PlaybackTelemetry{};
    m_decoderFrameCount  = 0;
    m_consumerFrameCount = 0;
    m_fpsStartTime = Clock::now();

    {
        std::lock_guard lock(m_threadMutex);
        if (!m_threadRunning && m_decoder && m_decoder->IsOpen()) {
            m_threadRunning = true;
            m_decoderThread = std::thread(&PlaybackController::DecoderThread, this);
            REX_LOG_INFO("Playback", "Decoder thread started");
        }
    }

    REX_LOG_INFO("Playback", "Playing");
}

void PlaybackController::Pause() {
    m_paused.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Playback", "Paused");
}

void PlaybackController::Stop() {
    m_playing.store(false, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_threadRunning = false;

    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    if (m_decoder && m_decoder->IsOpen()) {
        m_decoder->Seek(0.0);
    }

    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_writeCount.store(0, std::memory_order_relaxed);
    m_lastReadCount = 0;
    REX_LOG_INFO("Playback", "Stopped");
}

double PlaybackController::GetPosition() const {
    // Convert 100-ns timestamp to seconds for the public API
    return m_currentPosition.load(std::memory_order_relaxed);
}

// ─── Consumer (render thread) ────────────────────────────────────────────

DecodedFrame PlaybackController::Tick() {
    uint64_t wc = m_writeCount.load(std::memory_order_acquire);

    if (wc == 0 || wc == m_lastReadCount) {
        // No new frame since last Tick (SPSC invariant: this means the
        // producer hasn't written to our slot yet).
        return DecodedFrame{};
    }

    // ── Track dropped frames ──────────────────────────────────────────────
    // wc - m_lastReadCount > 1 means we missed at least one frame because
    // the decoder produced faster than we consumed.
    uint64_t skipped = (wc - m_lastReadCount) - 1;
    if (skipped > 0) {
        m_telemetry.framesDropped += skipped;
    }

    // Read slot (wc - 1) % kNumRingSlots — guaranteed distinct from the
    // producer's current slot (wc % 3) under the SPSC invariant.
    uint64_t slot = (wc - 1) % kNumRingSlots;
    const DecodedFrame& src = m_ring[slot];

    // Deep-copy pixel data (~8 MB for 1080p). The caller (Engine render
    // loop) must memcpy this into a GPU upload buffer anyway, so the
    // extra copy is unavoidable at this architecture level.
    DecodedFrame result = src;

    m_telemetry.framesConsumed++;
    m_consumerFrameCount++;
    m_lastReadCount = wc;

    // Update consumer FPS (periodically)
    auto now = Clock::now();
    double elapsed = std::chrono::duration<double>(now - m_fpsStartTime).count();
    if (elapsed >= 2.0) {
        m_telemetry.renderFps = static_cast<double>(m_consumerFrameCount) / elapsed;
        m_telemetry.decoderFps = static_cast<double>(m_decoderFrameCount) / elapsed;
        m_consumerFrameCount = 0;
        m_decoderFrameCount  = 0;
        m_fpsStartTime = now;
    }

    return result;
}

shared::Result<void> PlaybackController::Seek(double seconds) {
    std::lock_guard lock(m_threadMutex);

    if (!m_decoder || !m_decoder->IsOpen()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "No media open");
    }

    bool wasRunning = m_threadRunning.load();
    m_threadRunning = false;
    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    auto result = m_decoder->Seek(seconds);
    if (result.IsSuccess()) {
        m_currentPosition.store(seconds, std::memory_order_relaxed);
        m_writeCount.store(0, std::memory_order_relaxed);
        m_lastReadCount = 0;
    }

    if (wasRunning) {
        m_threadRunning = true;
        m_decoderThread = std::thread(&PlaybackController::DecoderThread, this);
    }

    return result;
}

// ─── Producer (decoder thread) ───────────────────────────────────────────

void PlaybackController::DecoderThread() {
    using namespace std::chrono;

    REX_LOG_INFO("Playback", "Decoder thread running");

    auto lastFrameTime = Clock::now();
    m_decoder->Seek(m_currentPosition.load(std::memory_order_relaxed));
    bool eof = false;

    while (m_threadRunning.load() && !eof) {
        if (m_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(milliseconds(10));
            continue;
        }

        // ── Pace to video frame rate ───────────────────────────────────
        double speed = m_speed.load(std::memory_order_relaxed);
        double interval = m_frameInterval;
        if (speed > 0.01f) {
            interval /= static_cast<double>(speed);
        }

        auto now = Clock::now();
        double elapsed = duration<double>(now - lastFrameTime).count();
        if (elapsed < interval) {
            double sleepMs = (interval - elapsed) * 1000.0;
            if (sleepMs > 0.5) {
                std::this_thread::sleep_for(milliseconds(static_cast<int>(sleepMs)));
            }
            continue;
        }
        lastFrameTime = now;

        // ── Decode one frame ───────────────────────────────────────────
        VideoFrame videoFrame = m_decoder->ReadFrame();

        // [Stage 2] 1Hz diagnostic: decoder output
        {
            static auto s_nextLog = std::chrono::steady_clock::now();
            auto s_now = std::chrono::steady_clock::now();
            if (s_now >= s_nextLog) {
                s_nextLog = s_now + std::chrono::seconds(1);
                auto& pix = videoFrame.pixels;
                uint32_t first4 = (pix.size() >= 4)
                    ? (static_cast<uint32_t>(pix[0]) << 0 |
                       static_cast<uint32_t>(pix[1]) << 8 |
                       static_cast<uint32_t>(pix[2]) << 16 |
                       static_cast<uint32_t>(pix[3]) << 24)
                    : 0;
                REX_LOG_INFO("PBDiag", "Decoded: valid={} {}x{} fmt={} ts={}ms bytes={} first4=0x{:08x} wc={}",
                             videoFrame.valid ? 1 : 0,
                             videoFrame.width, videoFrame.height,
                             static_cast<int>(videoFrame.format),
                             videoFrame.timestampHns / 10000,
                             pix.size(), first4,
                             m_writeCount.load(std::memory_order_relaxed));
            }
        }

        if (!videoFrame.valid) {
            // EOF — restart or stop
            if (m_looping.load(std::memory_order_relaxed)) {
                m_decoder->Seek(0.0);
                videoFrame = m_decoder->ReadFrame();
                if (!videoFrame.valid) {
                    REX_LOG_WARN("Playback", "EOF loop: decoder returned invalid frame");
                    continue;
                }
            } else {
                eof = true;
                m_playing.store(false, std::memory_order_release);
                continue;
            }
        }

        // ── Publish to ring buffer ─────────────────────────────────────
        // Under the SPSC invariant, the producer writes to slot (wc % 3)
        // while the consumer only reads from slot ((wc - 1) % 3), so
        // these indices are always distinct. No atomic CAS needed.
        uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
        int slot = static_cast<int>(wc % kNumRingSlots);

        {
            DecodedFrame& dst = m_ring[slot];

            // Move pixel data — avoids a large vector reallocation
            dst.pixels       = std::move(videoFrame.pixels);
            dst.width        = videoFrame.width;
            dst.height       = videoFrame.height;
            dst.rowPitch     = videoFrame.rowPitch;
            dst.format       = videoFrame.format;
            dst.timestampHns = videoFrame.timestampHns;
            dst.valid        = true;

            if (dst.pixels.empty() && dst.width > 0 && dst.height > 0) {
                dst.pixels.reserve(static_cast<size_t>(dst.width * dst.height * 4));
            }
        }

        // Release: all ring slot writes are visible before the counter
        // increment reaches the consumer.
        m_writeCount.store(wc + 1, std::memory_order_release);

        // [Stage 2] 1Hz diagnostic: ring buffer published
        {
            static auto s_nextLog = std::chrono::steady_clock::now();
            auto s_now = std::chrono::steady_clock::now();
            if (s_now >= s_nextLog) {
                s_nextLog = s_now + std::chrono::seconds(1);
                auto& dpix = m_ring[slot].pixels;
                uint32_t first4 = (dpix.size() >= 4)
                    ? (static_cast<uint32_t>(dpix[0]) << 0 |
                       static_cast<uint32_t>(dpix[1]) << 8 |
                       static_cast<uint32_t>(dpix[2]) << 16 |
                       static_cast<uint32_t>(dpix[3]) << 24)
                    : 0;
                REX_LOG_INFO("PBDiag", "Ring[slot={}]: wc={} valid={} {}x{} first4=0x{:08x}",
                             slot, wc + 1,
                             m_ring[slot].valid ? 1 : 0,
                             m_ring[slot].width, m_ring[slot].height, first4);
            }
        }

        // Track current position in seconds (from 100-ns timestamp)
        double posSec = static_cast<double>(videoFrame.timestampHns) / 10000000.0;
        m_currentPosition.store(posSec, std::memory_order_relaxed);

        m_telemetry.framesDecoded++;
        m_decoderFrameCount++;
    }

    REX_LOG_INFO("Playback", "Decoder thread ended (eof={})", eof);
}

} // namespace rex::wallpaper
