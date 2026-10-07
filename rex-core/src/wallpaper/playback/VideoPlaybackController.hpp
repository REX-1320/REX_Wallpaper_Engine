#pragma once

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/playback/FrameScheduler.hpp"
#include "wallpaper/audio/AudioPlayer.hpp"

#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace rex::wallpaper {

// ─── VideoPlaybackTelemetry ─────────────────────────────────────────────
//
// NOTE: intentionally distinct from PlaybackTelemetry (PlaybackController)
// to avoid a name collision while both controllers coexist in the binary.

struct VideoPlaybackTelemetry {
    uint64_t framesDecoded   = 0;
    uint64_t framesConsumed  = 0;
    uint64_t framesDropped   = 0;
    uint64_t gpuToCpuCopies  = 0;
    uint64_t cpuToGpuCopies  = 0;
    double   decoderFps      = 0.0;
    double   renderFps       = 0.0;
    // ─── Per-stage diagnostics (Phase 1) ──────────────────────────────
    double   sourceFps      = 0.0;  // stream frame rate (decoder-reported)
    double   convertFps     = 0.0;  // ConvertAndSignal+publish rate, same window
    uint64_t schedulerDrops = 0;    // frames skipped between decode & consume
                                    // (Phase 1 semantics: decoded-minus-
                                    // consumed; PTS-honest counting in Phase 2)
    uint64_t queueDepth     = 0;    // published-but-unconsumed frames now
    double   uploadLatencyMs = 0.0;
    // P13 per-stage percentiles (see DecoderDiagnostics).
    double   decodeMsP50 = 0.0, decodeMsP95 = 0.0, decodeMsP99 = 0.0;
    double   vpMsP50     = 0.0, vpMsP95     = 0.0, vpMsP99     = 0.0;
    uint32_t recoveryCount   = 0;
    uint32_t deviceResets    = 0;
    uint64_t ringSlotCount   = 0;
    uint64_t ringBytes       = 0;
    bool     hardwareAccelerated = false;
};

// ─── VideoPlaybackController ────────────────────────────────────────────
//
// FFmpeg-based playback controller. Decodes on a background thread paced
// to the source frame rate and publishes VideoFrames through the SPSC
// FrameScheduler. Frames are GPU-resident (D3D11 pool + shared handles),
// so the render thread never touches CPU buffers.
//
// Threading:
//   - Decode thread (internal): producer of FrameScheduler.
//   - Render thread (external): Tick() consumer of FrameScheduler.
//   - Control (Play/Pause/Stop/Seek/SetLoop): atomic or mutex-guarded.

class VideoPlaybackController {
public:
    VideoPlaybackController();
    ~VideoPlaybackController();

    VideoPlaybackController(const VideoPlaybackController&) = delete;
    VideoPlaybackController& operator=(const VideoPlaybackController&) = delete;

    // context11: immediate context of the same-adapter D3D11 device
    // created by VideoManager. fenceHandle: NT handle of the shared
    // D3D11/D3D12 fence.
    shared::Result<void> Open(std::string_view path, bool useHardware,
                              ID3D11DeviceContext* context11, HANDLE fenceHandle);
    void Close();

    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop);
    void SetSpeed(float speed);
    void SetVolume(float volume);
    void SetMute(bool mute);
    shared::Result<void> Seek(double seconds);

    /// Re-selects the decoder's video stream to best match the target
    /// resolution (adaptive resolution). Refreshes cached stream properties.
    void SelectBestStream(int targetWidth, int targetHeight);

    /// Returns the latest GPU frame since the last call, or nullptr.
    /// Caller must consume synchronously (wait on the slot fence before
    /// sampling) before the next Tick().
    const VideoFrame* Tick(double targetPts);

    bool IsOpen()    const { return m_open.load(std::memory_order_acquire); }
    bool IsPlaying() const { return m_playing.load(std::memory_order_acquire); }
    bool IsPaused()  const { return m_paused.load(std::memory_order_acquire); }

    double GetPosition() const { return m_position.load(std::memory_order_acquire); }
    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }

    // Forwarded from the decoder (shared-surface contract for D3D12).
    HANDLE GetSlotHandle(UINT slot) const;
    UINT64 GetSlotFence(UINT slot) const;

    // ─── Frame surface (cross-process preview) ───────────────────────────
    struct FrameSnapshot {
        UINT slot = 0;
        UINT64 fenceValue = 0;
        UINT width = 0;
        UINT height = 0;
        uint64_t frameIndex = 0;
    };
    UINT GetSlotCount() const { return static_cast<UINT>(m_scheduler.GetNumSlots()); }
    /// Latest published frame identity; safe from any thread.
    FrameSnapshot GetLastFrame() const;

    VideoPlaybackTelemetry GetTelemetry() const;

    /// Discontinuity rule (P1-5 / R10): a frame whose PTS runs strictly
    /// backward vs the last published one means the decoder seeked
    /// internally (loop wrap) — the scheduler must be cleared so the
    /// pre-seek frame is never presented. Equal PTS (frame repeats) is
    /// NOT a seek and must not clear.
    static bool IsBackwardJump(double lastPts, double framePts) {
        return framePts < lastPts;
    }

private:
    void DecoderThread();

    // ─── Owned workers (P1-8 / R15: single ownership mechanism) ──────
    // shared_ptr snapshots: every accessor copies under m_decodeMutex and
    // uses the copy, so Close() resetting the members can never
    // use-after-free a concurrent Seek/DecodeFrame/GetSlotHandle. The mutex
    // still serializes MUTATION (Seek vs DecodeFrame on one decoder).
    std::shared_ptr<FfmpegVideoDecoder> m_decoder;
    std::shared_ptr<AudioPlayer> m_audio;
    FrameScheduler m_scheduler;

    mutable std::mutex m_decodeMutex;  // serializes mutation; snapshots lifetime

    // P1-9: leaf mutex guarding ONLY the shared_ptr objects (copy/reset).
    // Never held across decoder calls: Close() snapshots here to
    // RequestStop() BEFORE joining, while the decode thread may sit blocked
    // in av_read_frame holding m_decodeMutex — taking that mutex pre-join
    // would deadlock. Order everywhere: m_decodeMutex, then m_decoderMutex.
    mutable std::mutex m_decoderMutex;
    std::shared_ptr<FfmpegVideoDecoder> SnapshotDecoder() const {
        std::lock_guard plock(m_decoderMutex);
        return m_decoder;
    }
    std::shared_ptr<AudioPlayer> SnapshotAudio() const {
        std::lock_guard plock(m_decoderMutex);
        return m_audio;
    }
    // P13 item 6: decode + publish frame #1 synchronously inside Open()
    // (also builds the surface pool before the decode thread starts).
    bool m_prewarmEnabled = true;

    // ─── Shared state ───────────────────────────────────────────────────
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_looping{true};
    std::atomic<float> m_speed{1.0f};
    std::atomic<double> m_position{0.0};

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    double m_frameIntervalMs = 33.333;
    int    m_width  = 0;
    int    m_height = 0;

    // Last published frame PTS (P1-5): backward jumps mean the decoder
    // seeked internally (loop wrap) → clear the scheduler. Always
    // accessed under m_decodeMutex (Seek, prewarm, decode thread).
    double m_lastPublishedPts = -1.0;

    // ─── Telemetry ──────────────────────────────────────────────────────
    std::atomic<uint64_t> m_framesDecoded{0};
    std::atomic<uint64_t> m_framesConsumed{0};
    std::atomic<uint64_t> m_framesDropped{0};
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_fpsStartTime;
    uint64_t m_decoderFrameCount  = 0;
    uint64_t m_consumerFrameCount = 0;
    uint64_t m_convertFrameCount  = 0;  // DecodeFrame-ok + published (Phase 1)

    // ─── Thread management ──────────────────────────────────────────────
    std::atomic<bool> m_threadRunning{false};
    std::thread m_thread;
    std::mutex m_threadMutex;

    // ─── Frame surface state ────────────────────────────────────────────
    HANDLE m_fenceHandle = nullptr;   // NT handle of the shared D3D12 fence
    mutable std::mutex m_lastFrameMutex;
    FrameSnapshot m_lastFrame;
};

} // namespace rex::wallpaper
