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

shared::Result<void> PlaybackController::Open(std::string_view path, bool useHardware) {
    std::lock_guard lock(m_threadMutex);

    if (m_decoder->IsOpen()) {
        m_threadRunning = false;
        if (m_decoderThread.joinable()) {
            m_decoderThread.join();
        }
        m_decoder->Close();
    }

    auto result = m_decoder->Open(path, useHardware);
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

    m_telemetry.ringSlotCount = kNumRingSlots;
    m_telemetry.ringBytes = static_cast<uint64_t>(kNumRingSlots) * frameBytes;

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
    m_telemetry.ringSlotCount = kNumRingSlots;
    m_telemetry.ringBytes     = static_cast<uint64_t>(kNumRingSlots) * m_width * m_height * 4;
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

const DecodedFrame* PlaybackController::Tick() {
    uint64_t wc = m_writeCount.load(std::memory_order_acquire);

    if (wc == 0 || wc == m_lastReadCount) {
        // No new frame since last Tick (SPSC invariant: this means the
        // producer hasn't written to our slot yet).
        return nullptr;
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
    m_lastReadCount = wc;

    m_telemetry.framesConsumed++;
    m_consumerFrameCount++;

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

    return &m_ring[slot];
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
        // Decode directly into the ring slot we are about to publish.
        // DecodedFrame& dst below is pre-reserved in Open(); the decoder
        // writes into dst.pixels without any intermediate allocation, so
        // this is a true zero-copy handoff (MF sample → CPU convert →
        // ring slot → GPU upload).
        uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
        int slot = static_cast<int>(wc % kNumRingSlots);
        DecodedFrame& dst = m_ring[slot];

        if (!m_decoder->ReadFrame(dst)) {
            // EOF — restart or stop
            if (m_looping.load(std::memory_order_relaxed)) {
                m_decoder->Seek(0.0);
                if (!m_decoder->ReadFrame(dst)) {
                    REX_LOG_WARN("Playback", "EOF loop: decoder returned invalid frame");
                    dst.valid = false;
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
        {
            dst.width        = m_decoder->GetWidth();
            dst.height       = m_decoder->GetHeight();
            dst.rowPitch     = m_decoder->GetWidth() * 4;
            dst.format       = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.valid        = true;

            if (dst.pixels.empty() && dst.width > 0 && dst.height > 0) {
                dst.pixels.reserve(static_cast<size_t>(dst.width * dst.height * 4));
            }
        }

        // Release: all ring slot writes are visible before the counter
        // increment reaches the consumer.
        m_writeCount.store(wc + 1, std::memory_order_release);

        // Track current position in seconds (from 100-ns timestamp)
        double posSec = static_cast<double>(dst.timestampHns) / 10000000.0;
        m_currentPosition.store(posSec, std::memory_order_relaxed);

        m_telemetry.framesDecoded++;
        m_decoderFrameCount++;
    }

    REX_LOG_INFO("Playback", "Decoder thread ended (eof={})", eof);
}

} // namespace rex::wallpaper
