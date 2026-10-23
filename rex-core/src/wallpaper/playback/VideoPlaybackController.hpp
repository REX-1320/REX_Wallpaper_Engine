#pragma once

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/playback/FrameScheduler.hpp"
#include "wallpaper/playback/CadenceController.hpp"
#include "wallpaper/playback/ContentRateMeter.hpp"
#include "wallpaper/audio/AudioPlayer.hpp"

#include <shared/Result.hpp>
#include <shared/PacingServo.hpp>

#include <windows.h>
#include <d3d11_4.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace rex::wallpaper {

// ─── A/V sync decision (pure) ──────────────────────────────────────────────
//
// Should the audio be re-seeked onto the video's position?
//
// The argument is the CHANGE in the audio-vs-video offset over time, not the
// offset itself: both positions lead their playhead by a fixed amount (the
// audio ring's depth, the video scheduler's slots), so the offset carries a
// constant bias that must not be mistaken for a sync error. Only a change
// beyond the threshold means the two clocks have really parted.
//
// Pure, so the decision is unit-tested without an audio device.
inline bool AvSyncNeedsCorrection(double driftChangeMs, double thresholdMs) {
    return driftChangeMs >= thresholdMs || driftChangeMs <= -thresholdMs;
}



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
    // ─── A/V sync watchdog ──────────────────────────────────────────────
    // The drift is the CHANGE in the audio-vs-video offset (both playheads
    // lead by a fixed amount, so the offset itself is not an error). A sync
    // error is silent by nature, so it has to be observable without ears.
    double   avSyncDriftMs    = 0.0;
    uint64_t avSyncCorrections = 0;  // times the watchdog had to re-seek
    // ─── Per-stage diagnostics (Phase 1) ──────────────────────────────
    double   sourceFps      = 0.0;  // stream frame rate (decoder-reported)
    double   convertFps     = 0.0;  // ConvertAndSignal+publish rate, same window
    uint64_t schedulerDrops = 0;    // frames skipped between decode & consume
                                    // (Phase 1 semantics: decoded-minus-
                                    // consumed; PTS-honest counting in Phase 2)
    uint64_t loopWraps       = 0;    // PTS wrap/seek events seen by the decode
                                    // thread (loop restart or explicit seek:
                                    // backward PTS jump -> scheduler Clear).
                                    // Separates wrap churn from steady state.
    uint64_t queueDepth     = 0;    // published-but-unconsumed frames now
    double   pacingMs       = 0.0;  // live decode-pacing interval (Phase 2
                                    // debug: m_frameIntervalMs / m_speed)
    double   playbackSpeed  = 1.0;  // content rate (Phase 3): decoderFps /
                                    // sourceFps; 1.0 = realtime
    double   uploadLatencyMs = 0.0;
    // Phase D decode-ahead reserve. reserveMs is the MEASURED depth of
    // already-decoded, not-yet-presented video - it is hard-capped by the
    // shared 8-slot pool, so it is frequently below the 200 ms nominal target
    // at high source rates. Report the measurement, never the target.
    double   reserveMs        = 0.0;
    double   decodeAheadRate  = 1.0;  // decoder rate / source rate
    uint64_t reserveUnderruns = 0;    // times the reserve hit empty
    // P13 per-stage percentiles (see DecoderDiagnostics).
    double   decodeMsP50 = 0.0, decodeMsP95 = 0.0, decodeMsP99 = 0.0;
    double   vpMsP50     = 0.0, vpMsP95     = 0.0, vpMsP99     = 0.0;
    // Decode pacing servo correction applied to the last sleep deadline
    // (negative pulls earlier). ~-0.5 ms steady-state on this machine.
    double   decodeServoCorrMs = 0.0;
    uint32_t recoveryCount   = 0;
    uint64_t sendEagainStalls = 0;  // packets abandoned at send-EAGAIN

    uint32_t hwDecoderProfiles = 0;   // driver HW decoder profile count
    // Drops per second over the telemetry window. schedulerDrops is a
    // LIFETIME counter and reads alarming forever after a startup burst
    // (it climbed to 694 while dropping nothing); the rate says what is
    // happening NOW.
    double   schedulerDropsPerSec = 0.0;
    // Largest drop burst seen inside the current window, so a spike the
    // average smooths away is still visible.
    double   schedulerDropsPeak = 0.0;
    // P2-1 unusable-path latch (decoder never produced a frame: zero
    // progress past budget, or send-fail streak on a never-decoded path).
    // The supervisor poison-parks on this; the reason is in runtime.log.
    bool     decodePathUnusable = false;
    // P0-2 content-rate check: seconds of CONTENT advanced per second of
    // wall clock, from presented PTS over a 10 s window (1.0 = realtime).
    // Position and playbackSpeed read healthy even when displayed content
    // runs at the wrong rate (the 2x defect); this metric comes from the
    // content itself and cannot lie that way. 0.0 until the first window.
    double   contentRate = 0.0;
    uint32_t deviceResets    = 0;
    uint64_t ringSlotCount   = 0;
    uint64_t ringBytes       = 0;
    bool     hardwareAccelerated = false;
    // ─── Scene end ───────────────────────────────────────────────────────
    // True once the scene's content reached its end (the video played to
    // its end). The latch is one-shot: set by the frame that reached the
    // end, re-armed by Open/Seek/Stop, so it reports ONE scene end per
    // latch. The app's playlist engine needs this fact to change the
    // wallpaper "when the current ends" (the dead media bridge could
    // never see it).
    bool     sceneEnded = false;
};

// ─── Scene-end latch ─────────────────────────────────────────────────────
//
// The one-shot "the scene played to its end" signal the app's playlist uses
// for its "when the current ends" change modes. The latch is SET by the
// first frame that reached the end of the content and RE-ARMED by every
// seek / scene load / stop.
//
// Re-arming on seek is load-bearing: the app's playlist change re-seeks the
// controller (VideoManager's cache-hit path), and a latch that survived the
// seek would report the SAME end again - the change loop would run away:
//     change -> seek -> ENDED -> change -> seek -> ...
//
// A backward PTS jump (a seek, a loop wrap) can never SET it: the
// comparison only grows with forward progress, so a seek can never look
// like "the video ended".
class SceneEndLatch {
public:
    bool Ended() const { return m_ended.load(std::memory_order_acquire); }

    /// Re-arm: the next end is a NEW scene end.
    void Rearm() { m_ended.store(false, std::memory_order_release); }

    /// Latch the end from a fact other than a PTS sample: the decoder has
    /// exhausted the video stream. Necessary when the container (usually an
    /// audio tail) runs past the last video frame, which the PTS comparison
    /// against the container duration can never reach.
    void MarkEnded() { m_ended.store(true, std::memory_order_release); }

    /// Feed one published or presented frame. `framePts` is the ABSOLUTE
    /// stream PTS; the content spans [contentStart, contentStart+duration].
    /// Returns true only for the frame that SETS the latch (the rising
    /// edge), so the caller can log one line per scene end.
    bool OnFrame(double framePts, double contentStart, double duration,
                 double frameRate) {
        if (m_ended.load(std::memory_order_relaxed)) return false;
        if (!ReachedEndOfContent(framePts - contentStart, duration, frameRate)) {
            return false;
        }
        m_ended.store(true, std::memory_order_release);
        return true;
    }

    /// End-of-content decision (pure, unit-tested).
    ///
    /// The published PTS of a stream's LAST frame sits ONE FRAME SHORT of
    /// the duration (the position cannot express "finished" - see
    /// NeedsRewindOnPlay), so the content ended when the frame's PTS plus
    /// the frame's own duration reaches the duration. The comparison
    /// carries a one-frame tolerance because the container's duration may
    /// sit a frame beyond the last published PTS and the frame rate is
    /// measured, not declared.
    static bool ReachedEndOfContent(double framePts, double duration,
                                    double frameRate) {
        if (duration <= 0.0 || framePts < 0.0) return false;
        const double frameDuration = frameRate > 0.0 ? 1.0 / frameRate : 0.0;
        return framePts + frameDuration >= duration - frameDuration;
    }

private:
    std::atomic<bool> m_ended{false};
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
    /// Request an audio output layout (auto/mono/stereo/quad/5.1). The
    /// endpoint negotiates downward when it cannot honour the request.
    void SetAudioOutputMode(AudioOutputMode mode);
    /// Render thread finished consuming frameIndex — releases its pool slot
    /// lease in the decoder (Phase D slot-lifetime prerequisite).
    void NotifyFrameConsumed(int64_t frameIndex) {
        // P1-8 discipline: snapshot under the leaf mutex — Close() resets
        // m_decoder, so a raw read here could use-after-free on a scene
        // switch racing the render thread's per-frame report.
        if (auto decoder = SnapshotDecoder()) {
            decoder->NotifyFrameConsumed(frameIndex);
        }
    }
    void SetMute(bool mute);
    /// Mute state snapshot (P1-8: under m_decodeMutex like all member
    /// access). Proves the single-audio-owner policy per monitor video.
    bool IsMuted() const;
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

    /// A/V sync telemetry. The drift is the CHANGE in the audio-vs-video
    /// offset since the last baseline (see the watchdog below) - the offset
    /// itself is not an error, because both playheads lead by a fixed
    /// amount. A drift that stays under kAvSyncCorrectionMs (250) is not
    /// corrected; the counter says how often the watchdog had to act.
    double GetAvSyncDriftMs() const {
        return m_avSyncDriftMs.load(std::memory_order_relaxed);
    }
    uint64_t GetAvSyncCorrections() const {
        return m_avSyncCorrections.load(std::memory_order_relaxed);
    }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }

    /// True once the scene's content reached its end (the video played to
    /// its end). One-shot: re-armed by Open/Seek/Stop, so a seek - the
    /// app's playlist change re-seeks the controller - can never look like
    /// an end (that would run the change loop away:
    /// change -> seek -> ENDED -> change -> seek -> ...).
    bool SceneEnded() const { return m_sceneEnd.Ended(); }

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

    /// Discontinuity rule (P1-5 / R10, Phase 2 refinement): a frame whose
    /// PTS runs strictly backward vs the last published one means the
    /// decoder seeked internally (loop wrap) — the scheduler must be
    /// cleared so the pre-seek frame is never presented. Equal PTS
    /// (frame repeats) is NOT a seek and must not clear. NEITHER is
    /// ordinary B-frame reorder jitter: decode-order PTS steps backward
    /// by a frame or two on every B-frame (proven via packet trace:
    /// 0.033 -> 0.016 -> 0.066 -> 0.050 ...), so only a backward jump
    /// that is large (> 1 s) or lands at stream start (~0) counts.
    static bool IsBackwardJump(double lastPts, double framePts) {
        if (!(framePts < lastPts)) {
            return false;
        }
        return (lastPts - framePts > 1.0) || (framePts <= 1e-9);
    }

    /// Loop-relative wall target (Phase 2): a looping decoder wraps its
    /// PTS every duration seconds while the wall clock marches on. The
    /// scheduler compares PTS against the wall, so the target must wrap
    /// with the content — otherwise nothing is ever due past the first
    /// loop and the render loop starves while drops explode. Pure.
    static double WrapTarget(double targetPts, double duration, bool looping) {
        if (!looping || duration <= 0.0 || targetPts < 0.0) {
            return targetPts;
        }
        double wrapped = std::fmod(targetPts, duration);
        if (wrapped < 0.0) {
            wrapped += duration;
        }
        return wrapped;
    }

    /// Play()-time restart decision (audit 2026-10-17, §5.4 #5): true when
    /// the playhead must rewind to 0 before (re)starting. The position
    /// heuristic alone cannot see a natural end of stream — the last
    /// published PTS stops one frame short of the duration — so a
    /// non-looping video that played to completion could never restart.
    /// EOS is latched explicitly by the decode thread and ORed in. Pure;
    /// unit-tested.
    static bool NeedsRewindOnPlay(bool eosReached, double position,
                                  double duration) {
        return eosReached || position >= duration - 0.001;
    }

    /// End-of-content decision (pure, unit-tested). Defined by the latch so
    /// the decision and the latch state machine are tested together.
    static bool ReachedEndOfContent(double framePts, double duration,
                                    double frameRate) {
        return SceneEndLatch::ReachedEndOfContent(framePts, duration,
                                                  frameRate);
    }

private:
    void DecoderThread();
    // Re-scale the decode-ahead policy from the CURRENT m_frameRate and
    // re-apply the env overrides (audit 5.4 #13: an adaptive stream switch
    // changed the source rate but kept the open-time reserve marks).
    void RefreshDecodeAheadPolicy(bool logOverrides);

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
    // EOS latch (audit 2026-10-17, §5.4 #5): set by the decode thread when
    // a NON-looping stream ends; cleared by Open/Seek/Stop. Play() ORs it
    // into the restart decision because the published position cannot
    // express "finished" (last PTS is one frame short of the duration).
    std::atomic<bool> m_eosReached{false};
    std::atomic<bool> m_looping{true};
    // Scene-end latch: one-shot "the scene played to its end" signal.
    // Re-armed by Open/Seek/Stop - every seek re-arms it, so "the current
    // ended" is exactly one scene end and never a replay of the previous
    // scene's end (the change loop would run away otherwise).
    SceneEndLatch m_sceneEnd;
    std::atomic<float> m_speed{1.0f};
    // Phase D: measured reserve depth (ms) and the decode-ahead policy in force.
    std::atomic<double> m_reserveMs{0.0};
    // Counts frames that had to be REPEATED because the reserve was empty and
    // no new decode had landed - i.e. a genuine reserve underrun, distinct
    // from ordinary pacing repeats.
    std::atomic<uint64_t> m_reserveUnderruns{0};
    CadenceController::DecodeAheadPolicy m_decodeAheadPolicy{};
    std::atomic<double> m_position{0.0};
    // ── A/V sync watchdog ───────────────────────────────────────────────
    // The drift is measured as the CHANGE in the audio-vs-video offset
    // (both positions lead their playhead by a fixed amount, so the offset
    // itself is not the error). kAvSyncCorrectionMs is far above any
    // scheduling artefact, so the watchdog can only fire on a real drift.
    static constexpr int kAvSyncCheckFrames = 120;    // ~4 s at 30 fps
    static constexpr double kAvSyncCorrectionMs = 250.0;
    int m_avSyncTicks = 0;
    double m_avSyncBaseline = 0.0;
    bool m_avSyncBaselineValid = false;
    std::atomic<double> m_avSyncDriftMs{0.0};
    std::atomic<uint64_t> m_avSyncCorrections{0};

    /// A/V sync watchdog: measure the drift between the audio's playhead and
    /// the video's and correct it when the two clocks really part.
    void CheckAvSync();

    // Decode pacing servo: cancels the steady per-sleep wake overshoot
    // (~+0.5 ms on this machine: ~3% slow at 60 fps, ~6% at 120 fps) that an
    // open-loop interval can never recover — the reserve pins negative and
    // catch-up burst cycles forever. Same PacingServo class as the render
    // loop (shared/, unit-tested). Rollback: REX_DECODE_SERVO=0.
    PacingServo m_decodeServo{};
    std::atomic<double> m_decodeServoCorrMs{0.0};
    bool m_decodeServoEnabled = true;
    std::chrono::steady_clock::time_point m_prevDecodeStart{};
    bool m_prevDecodeValid = false;
    // Per-frame stage CSV throttle (diagnostic): last dump time, decode-
    // thread owned. Dumps only when REX_PERFRAME_CSV=1 (see DecoderThread).
    // (Full type: the Clock alias is declared further below.)
    std::chrono::steady_clock::time_point m_lastStageDump{};

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    double m_frameIntervalMs = 33.333;
    // Stream start timestamp (seconds): the frame PTS are ABSOLUTE stream
    // timestamps, so the content spans [m_contentStart, m_contentStart +
    // m_duration]. The scene-end decision is taken on the content-relative
    // position, or a file with a non-zero start_time would look like it
    // ended before it began.
    double m_contentStart = 0.0;
    int    m_width  = 0;
    int    m_height = 0;

    // Last published frame PTS (P1-5): backward jumps mean the decoder
    // seeked internally (loop wrap) → clear the scheduler. Always
    // accessed under m_decodeMutex (Seek, prewarm, decode thread).
    double m_lastPublishedPts = -1.0;

    // Decode generation (Stop/Seek race): bumped under m_decodeMutex by
    // Seek/Stop. The decode thread snapshots it before DecodeFrame and
    // publishes only when unchanged — a frame decoded from the pre-seek
    // stream must never overwrite the post-seek position.
    uint64_t m_decodeGeneration{0};

    // ─── Telemetry ──────────────────────────────────────────────────────
    std::atomic<uint64_t> m_framesDecoded{0};
    std::atomic<uint64_t> m_framesConsumed{0};
    std::atomic<uint64_t> m_framesDropped{0};
    // Windowed drop-rate sampler (see GetTelemetry).
    mutable uint64_t m_dropRateBase{0};
    mutable std::chrono::steady_clock::time_point m_dropRateTime{};
    mutable double m_dropRatePerSec{0.0};
    mutable double m_dropRatePeak = 0.0;
    // P0-2 content-rate meter: fed by Tick() on the render thread, folded by
    // GetTelemetry() on the 5 s sampler thread. Own leaf mutex - never held
    // while acquiring another lock (Stop/Seek take it only inside their own
    // critical sections).
    mutable std::mutex m_contentMutex;
    mutable ContentRateMeter m_contentMeter;
    // P0-4 backpressure: latest loop-relative wall target seen by Tick()
    // (render thread). The decode thread differences it against the published
    // position to get the playhead-relative lead that paces decode. Plain
    // atomic, lock-free on x64; a torn/stale sample only mistunes one pacing
    // iteration (the policy clamps and self-corrects every loop).
    std::atomic<double> m_lastTargetPts{0.0};
    std::atomic<uint64_t> m_loopWraps{0};  // backward-PTS-jump events (loop/seek)
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
