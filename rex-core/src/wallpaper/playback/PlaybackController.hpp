#pragma once

#include "wallpaper/decoder/MediaFoundationDecoder.hpp"

#include <shared/Result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::wallpaper {

// ─── PlaybackTelemetry ─────────────────────────────────────────────────────

struct PlaybackTelemetry {
    uint64_t framesDecoded   = 0;  // total frames produced by decoder thread
    uint64_t framesConsumed  = 0;  // total frames consumed by Tick()
    uint64_t framesDropped   = 0;  // frames overwritten before being consumed
    double   decoderFps      = 0.0; // smoothed decode rate
    double   renderFps       = 0.0; // smoothed render consume rate
    double   uploadLatencyMs = 0.0; // time from decode → GPU upload start
    uint32_t recoveryCount   = 0;
    uint32_t deviceResets    = 0;
    uint64_t ringSlotCount   = 0;  // number of ring slots
    uint64_t ringBytes       = 0;  // sum of slot pixel buffer capacities
};

// ─── DecodedFrame ─────────────────────────────────────────────────────────
//
// Defined in MediaFoundationDecoder.hpp (the canonical frame type across
// the decode → ring → render pipeline). Metadata is preserved from the
// decoder so downstream consumers can handle HDR, tone-mapping, or a/v
// sync without re-probing the source.

// ─── PlaybackController ──────────────────────────────────────────────────
//
// Decodes video frames on a background thread and transfers them to the
// render thread via a lock-free triple-buffered ring buffer.
//
// ─── Threading model ────────────────────────────────────────────────────
//
// **SINGLE PRODUCER, SINGLE CONSUMER (SPSC).**
//
// This class assumes exactly one decoder thread (producer) and exactly one
// render/consumer thread. The ring buffer synchronization relies on this
// SPSC guarantee — the producer writes to slot (wc % kNumRingSlots) and
// the consumer reads from slot ((wc - 1) % kNumRingSlots). With the SPSC
// invariant, these are always distinct indices, so no mutex is needed on
// the hot path — only atomic load-acquire / store-release on m_writeCount.
//
// **If multiple decoders or consumers are ever needed, the ring buffer
// synchronization MUST be redesigned** (e.g., one ring per decoder + a
// mux, or a multi-producer queue).
//
// ─── Ring buffer safety ─────────────────────────────────────────────────
//
// With 3 slots, producer writes to slot (wc % 3) and consumer reads from
// slot ((wc - 1) % 3). These are always distinct indices, preventing torn
// reads. The producer may overwrite a slot the consumer hasn't read yet,
// which is tracked as a dropped frame in telemetry — the consumer simply
// picks up the next-latest frame.
//
// ─── Shared state access ────────────────────────────────────────────────
//
// Play():             writes m_playing, m_paused (atomic, no lock)
// Pause():            writes m_paused (atomic, no lock)
// Stop():             joins decoder thread under m_threadMutex
// Tick():             atomic load of m_writeCount + direct read (RC, no lock)
// Open() / Close():   exclusive under m_threadMutex

class PlaybackController {
public:
    PlaybackController();
    ~PlaybackController();

    // ─── Lifecycle ──────────────────────────────────────────────────────
    shared::Result<void> Open(std::string_view path, bool useHardware = true);
    void Close();

    // ─── Playback Control ───────────────────────────────────────────────
    void Play();
    void Pause();
    void Stop();

    // ─── Runtime Adjustments ────────────────────────────────────────────
    // Thread-safe: consumed by the decoder thread via atomics.
    void SetSpeed(float speed) { m_speed.store(speed, std::memory_order_relaxed); }
    void SetLoop(bool loop) { m_looping.store(loop, std::memory_order_relaxed); }

    // ─── Frame Tick ─────────────────────────────────────────────────────
    /// Returns a pointer to the latest decoded frame, or nullptr if no
    /// new frame has been decoded since the last call. Never blocks.
    ///
    /// Lifetime: the returned pointer references a ring-buffer slot that
    /// the producer will not write again until three more frames are
    /// published (SPSC guarantee). The caller must consume it
    /// synchronously (e.g. GPU upload) before the next Tick().
    const DecodedFrame* Tick();

    shared::Result<void> Seek(double seconds);

    // ─── State ──────────────────────────────────────────────────────────
    bool   IsPlaying() const { return m_playing.load(); }
    bool   IsPaused()  const { return m_paused.load(); }
    double GetPosition() const;
    double GetDuration() const { return m_duration; }
    int    GetWidth()  const { return m_width; }
    int    GetHeight() const { return m_height; }
    bool   IsOpen()    const { return m_open.load(); }

    // ─── Telemetry ──────────────────────────────────────────────────────
    PlaybackTelemetry GetTelemetry() const { return m_telemetry; }

private:
    void DecoderThread();

    std::unique_ptr<MediaFoundationDecoder> m_decoder;

    // ─── Ring Buffer (SPSC, 3 slots) ────────────────────────────────────
    static constexpr int kNumRingSlots = 3;
    DecodedFrame          m_ring[kNumRingSlots];
    std::atomic<uint64_t> m_writeCount{0};
    uint64_t              m_lastReadCount = 0;   // consumer-local

    // ─── Shared State ───────────────────────────────────────────────────
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_looping{true};
    std::atomic<float> m_speed{1.0f};
    std::atomic<double> m_currentPosition{0.0};

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    double m_frameInterval = 0.0;
    int    m_width     = 0;
    int    m_height    = 0;

    // ─── Telemetry ──────────────────────────────────────────────────────
    PlaybackTelemetry m_telemetry{};
    // Smoothed FPS tracking
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point m_fpsStartTime;
    uint64_t m_decoderFrameCount  = 0;
    uint64_t m_consumerFrameCount = 0;

    // ─── Thread Management ──────────────────────────────────────────────
    std::atomic<bool> m_threadRunning{false};
    std::thread m_decoderThread;

    std::mutex m_threadMutex;
};

} // namespace rex::wallpaper
