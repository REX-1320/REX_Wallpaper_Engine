#pragma once

#include <shared/Result.hpp>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
}

namespace rex::wallpaper {

// ─── Audio output layout ────────────────────────────────────────────────
//
// The DEVICE layout is what WASAPI renders, and it is negotiated against the
// endpoint: requesting more channels than the endpoint's mix format provides
// makes the engine period describe a layout the endpoint never hands back, and
// the render step then overruns its buffer (this faulted the runtime on every
// AC-3/E-AC-3 track). Audio is therefore always mixed to the negotiated
// layout, never to the source layout.
enum class AudioOutputMode {
    Auto = 0,      // match the source, capped by what the endpoint supports
    Mono,          // 1
    Stereo,        // 2
    Quad,          // 4  (FL FR SL SR)
    Surround51,    // 6  (FL FR FC LFE SL SR)
};

/// Channel count for a mode (0 for Auto, which resolves per endpoint).
int AudioModeChannels(AudioOutputMode mode);
/// Inverse of AudioModeChannels; unknown counts resolve to Stereo.
AudioOutputMode AudioChannelsToMode(int channels);
/// Human-readable name, for logs and the UI.
const char* AudioModeName(AudioOutputMode mode);

// ─── AudioPlayer ─────────────────────────────────────────────────────────
//
// Decodes the audio stream of a media file (its OWN AVFormatContext — never
// shared with FfmpegVideoDecoder, which reads packets from a single context
// on the video decode thread) and plays it through WASAPI in shared mode
// with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, so any PCM sample rate / channel
// count is accepted and the audio engine performs the resampling.
//
// Threading: one internal thread owns every FFmpeg and WASAPI call (COM is
// initialized on that thread). Control methods (Play/Pause/Stop/Seek/
// SetLoop/SetVolume/SetMute) only flip atomics or stash values, so they are
// safe from any thread. A ring buffer of interleaved int16 samples sits
// between the decoder (producer) and WASAPI (consumer) inside that thread.
//
// No audio stream in the file is NOT an error: Open returns a generic
// success with HasAudio() == false so callers can treat it as non-fatal.

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /// Opens the file's audio stream and the default render endpoint.
    /// Returns Ok with HasAudio()==false when the file has no audio.
    shared::Result<void> Open(std::string_view path);
    void Close();
    bool IsOpen() const { return m_open.load(std::memory_order_acquire); }
    bool HasAudio() const { return m_hasAudio; }

    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop);
    /// Re-seeks the audio demuxer; takes effect on the internal thread.
    void Seek(double seconds);
    void SetVolume(float volume);  // 0..1, applied on render
    void SetMute(bool mute);
    /// Mute state (single-audio-owner policy proof + UI surfacing).
    bool IsMuted() const { return m_muted.load(std::memory_order_acquire); }
    /// Playback tempo (P1-13 / R20): the render step consumes speed x device
    /// frames of content per callback (linear resample), so audio stays in
    /// sync with video-paced playback. Rate resample, not pitch-corrected:
    /// pitch follows speed (1.5x sounds sharper). Clamped to 0.25..4.
    void SetSpeed(float speed);

    /// Linear-interpolation resample of interleaved S16 (P1-13): maps
    /// srcFrames content frames onto dstFrames device frames, per channel.
    /// Endpoints exact; no overflow (float math + S16 clamp). Pure function.
    static void ResampleLinear(const int16_t* src, size_t srcFrames,
                               int channels, int16_t* dst, size_t dstFrames);

    // ─── Testable seams ────────────────────────────────────────────────
    // The decode loop and the sample conversion are exposed as pure
    // functions so the state machine and the channel mix can be verified
    // without a live WASAPI endpoint. Production uses these exact functions,
    // so a test failure is a production failure — not a copy of it.

    /// FFmpeg send/receive state machine: submits `pkt` and delivers every
    /// frame the decoder produces. If the decoder rejects the packet with
    /// EAGAIN (its output was not drained), it drains first and RETRIES THE
    /// SAME PACKET — FFmpeg requires the rejected packet to be resubmitted,
    /// and unreffing it instead silently loses audio.
    /// Returns frames delivered (>= 0) or a negative AVERROR.
    static int SubmitPacketAndDrain(AVCodecContext* dec, AVPacket* pkt,
                                    AVFrame* frame,
                                    const std::function<void(AVFrame*)>& sink);

    /// Delivers every frame the decoder currently has ready, repeating
    /// avcodec_receive_frame until EAGAIN/EOF. A single submitted packet may
    /// produce 0 or more than one frame. Returns frames delivered, or a
    /// negative AVERROR on a terminal decode failure.
    static int DrainAvailableFrames(AVCodecContext* dec, AVFrame* frame,
                                    const std::function<void(AVFrame*)>& sink);

    /// Converts one decoded frame (any sample format, planar or interleaved)
    /// to interleaved S16 at `dstChannels`, applying the 5.1/7.1 -> stereo
    /// downmix coefficients instead of discarding the surround channels.
    /// Writes at most `dstCapacityFrames`; returns frames written, or
    /// negative AVERROR if the sample format is unsupported.
    static int ConvertFrameToS16(const AVFrame* frame, int dstChannels,
                                 int16_t* dst, size_t dstCapacityFrames);

    /// Downmix coefficient applied to `srcChannel` when folding a source of
    /// `srcChannels` into `dstChannels`. Returns 0 when the channel does not
    /// feed that output (this is how LFE stays out of the stereo mix).
    /// Exposed so tests can assert the exact matrix rather than "some audio
    /// came out".
    static float GetDownmixCoefficients(int srcChannels, int dstChannels,
                                        int srcChannel, int dstChannel);

    /// Selects the device channel layout. Safe from any thread; takes effect
    /// at the next endpoint negotiation (immediately when audio is open).
    /// If the endpoint cannot honour the request, playback falls back to the
    /// next-best layout and DidFallbackToSmallerMode() reports it so the UI
    /// can tell the user rather than silently ignoring the choice.
    void SetOutputMode(AudioOutputMode mode);
    AudioOutputMode GetRequestedMode() const {
        return m_requestedMode.load(std::memory_order_acquire);
    }
    /// The layout actually in use after negotiation.
    AudioOutputMode GetActiveMode() const {
        return m_activeMode.load(std::memory_order_acquire);
    }
    /// True when GetRequestedMode() could not be honoured and a smaller
    /// layout is in use. Cleared by the next successful negotiation.
    bool DidFallbackToSmallerMode() const {
        return m_modeFallback.load(std::memory_order_acquire);
    }
    /// Negotiated device channel count (matches the ring layout).
    int GetDeviceChannels() const { return m_channels; }

    // Output telemetry. Audio correctness cannot be judged by ear in a
    // headless test, so the render path reports what it actually pushed to
    // the endpoint and how often it ran dry.
    uint64_t GetRenderedFrames() const {
        return m_renderedFrames.load(std::memory_order_relaxed);
    }
    uint64_t GetDecodedFrames() const {
        return m_decodedFrames.load(std::memory_order_relaxed);
    }
    uint64_t GetUnderruns() const {
        return m_underruns.load(std::memory_order_relaxed);
    }

    /// Picks the layout to render: the widest supported one that satisfies
    /// both the request and what the endpoint's mix format provides.
    /// Returns 0 when the exact request is impossible (the caller then steps
    /// down and reports the fallback). Exposed for tests.
    static int NegotiateChannels(AudioOutputMode requested, int sourceChannels,
                                 int endpointMixChannels);

    int  GetSampleRate() const { return m_sampleRate; }
    int  GetChannels()   const { return m_channels; }
    double GetPosition() const { return m_position.load(std::memory_order_acquire); }

private:
    void ThreadLoop();

    // ─── Demux / decode (internal thread only) ──────────────────────────
    bool DecodeNextChunk();
    /// Fills the ring to ~150 ms of audio before the WASAPI stream starts.
    /// True when primed, or when the source is too short to prime (starting
    /// is still correct then — it simply has no headroom).
    bool PrimedForStart();
    bool RewindToStart();
    void DoSeek(double seconds);
    int  PushFrameToRing(AVFrame* frame);

    // ─── WASAPI (internal thread only) ──────────────────────────────────
    void RenderStep();
    /// (Re)open the default render endpoint. True when rendering is
    /// possible; false keeps decode running while render retries.
    bool OpenEndpoint();
    /// Resizes the ring to `channels` and resets its positions. Internal
    /// thread only, and only while the stream is stopped.
    void ResizeRing(int channels);

    // ─── FFmpeg state ───────────────────────────────────────────────────
    AVFormatContext* m_fmtCtx    = nullptr;
    AVCodecContext*  m_decCtx    = nullptr;
    AVFrame*         m_frame     = nullptr;
    AVPacket*        m_pkt       = nullptr;
    int  m_streamIndex           = -1;
    bool m_eof                   = false;
    std::string m_codecName;     // for error logs (e.g. "eac3")
int  m_sampleRate            = 48000;
    int  m_channels            = 2;   // DEVICE layout (negotiated)
    int  m_srcChannels         = 2;   // codec layout (6 for AC-3/E-AC-3)
    // Requested vs negotiated layout. Atomic: SetOutputMode may be called
    // from the UI thread while the audio thread negotiates.
    std::atomic<AudioOutputMode> m_requestedMode{AudioOutputMode::Stereo};
    std::atomic<AudioOutputMode> m_activeMode{AudioOutputMode::Stereo};
    std::atomic<bool> m_modeFallback{false};
    std::atomic<bool> m_modeChanged{false};  // set by SetOutputMode
    int  m_endpointMixChannels = 2;          // probed per endpoint open
    int  m_bytesPerSample        = 2;

    // ─── WASAPI state (internal thread only) ────────────────────────────
    Microsoft::WRL::ComPtr<IAudioClient>       m_client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> m_render;
    UINT32 m_bufferFrames = 0;

    // ─── Ring buffer (frames of interleaved int16) ──────────────────────
    std::vector<int16_t> m_ring;
    std::vector<int16_t> m_scratch;  // conversion staging (decode thread)
    size_t m_ringFrames   = 0;   // capacity in frames
    size_t m_readPos      = 0;   // frames
    size_t m_writePos     = 0;
    size_t m_bufferedFrames = 0;
    mutable std::mutex m_ringMutex;  // guard against control-thread Clear()

    // ─── Control state (atomics) ────────────────────────────────────────
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_looping{true};
    std::atomic<bool> m_muted{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_speed{1.0f};
    // Fractional content-frame debt for tempo resample (< 1 frame always;
    // keeps long-term rate exact across callbacks). Under m_ringMutex.
    double m_speedError = 0.0;
    std::atomic<bool> m_seekRequested{false};
    std::atomic<double> m_seekTime{0.0};
    std::atomic<double> m_position{0.0};
    // P3-G4: set by RenderStep on endpoint-loss HRs; the thread loop
    // re-enumerates instead of silencing audio until reload.
    std::atomic<bool> m_endpointLost{false};
    // Render-path telemetry (see getters).
    std::atomic<uint64_t> m_renderedFrames{0};
    // Signed peak handed to the endpoint in the current window (S16 units).
    // Healthy counters with peak 0 means WE rendered silence; nonzero peak
    // the user cannot hear means the OS routed it elsewhere.
    std::atomic<int> m_peakWindow{0};
    std::atomic<uint64_t> m_decodedFrames{0};
    std::atomic<uint64_t> m_underruns{0};
    bool m_hasAudio = false;

    std::thread m_thread;
};

} // namespace rex::wallpaper
