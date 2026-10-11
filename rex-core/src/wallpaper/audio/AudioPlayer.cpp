#include "wallpaper/audio/AudioPlayer.hpp"

#include <shared/Logger.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

#include <avrt.h>
#pragma comment(lib, "avrt.lib")

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "AudioPlayer";
constexpr size_t kMinFreeChunkFrames = 2048;  // pace decode when ring is this full
constexpr size_t kRingSeconds = 1;            // ring capacity in seconds of audio
constexpr int    kPrefillMs    = 150;          // audio headroom before WASAPI Start
constexpr int    kPrefillTimeoutMs = 2000;    // bounded: never stall startup

inline int16_t ClampToS16(float v) {
    if (v <= -1.0f) return INT16_MIN;
    if (v >= 1.0f)  return INT16_MAX;
    return static_cast<int16_t>(v * 32767.0f);
}

/// HRs that mean the render endpoint is gone (unplug/default change),
/// not a transient glitch: the client must be re-enumerated.
inline bool IsEndpointLost(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

/// FFmpeg error code -> text. NOT av_err2str: that is a GNU statement
/// expression `({ ... })`, which MSVC rejects outright (C4576) — every use
/// of it silently prevented this file from compiling.
std::string AvErrStr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// ─── Downmix matrix ───────────────────────────────────────────────────────
//
// The ring is always laid out at the DEVICE channel count. Previously a frame
// carrying more channels than the device was "downmixed" by reading only the
// first `channels` of it — a 5.1 track lost its centre and surrounds and
// sounded hollow. These are the standard Dolby / ITU-R BS.775 coefficients.
//
// LFE is deliberately EXCLUDED (no coefficient): the standard stereo downmix
// drops it rather than folding sub-bass into full-range channels. Tests assert
// this, so the behaviour cannot silently regress to "add everything".
constexpr float kSurroundAtten   = 0.5f;      // -6 dB
constexpr float kCentreToStereo = 0.7071f;   // -3 dB
constexpr float kDirectToStereo = 0.7071f;

} // namespace

// Channel orders (FFmpeg): 5.1 = FL FR FC LFE SL SR
//                          7.1 = FL FR FC LFE BL BR SL SR
float AudioPlayer::GetDownmixCoefficients(int srcChannels, int dstChannels,
                                          int srcChannel, int dstChannel) {
    if (srcChannels <= 0 || dstChannels <= 0) return 0.0f;
    if (srcChannel < 0 || srcChannel >= srcChannels) return 0.0f;

    if (dstChannels == 1) return 1.0f;   // mono out: every channel folds in
    if (srcChannels == dstChannels) return 1.0f;  // identity
    if (dstChannels != 2) {
        // No matrix defined for this pair: pass the source channel straight
        // through when it exists, otherwise contribute nothing.
        return dstChannel < srcChannels ? 1.0f : 0.0f;
    }

    if (srcChannels == 1) return 1.0f;   // mono -> both outputs
    if (srcChannels == 2) return dstChannel < 2 ? 1.0f : 0.0f;  // stereo

    if (srcChannels == 4) {             // quad: FL FR BL BR
        if (srcChannel == 0 && dstChannel == 0) return 1.0f;
        if (srcChannel == 2 && dstChannel == 0) return kSurroundAtten;
        if (srcChannel == 1 && dstChannel == 1) return 1.0f;
        if (srcChannel == 3 && dstChannel == 1) return kSurroundAtten;
        return 0.0f;
    }

    // 5.1 / 7.1: front + centre into both, surrounds into their own side.
    const bool is71 = (srcChannels >= 7);
    const int centre  = 2;
    const int sl = is71 ? 6 : 4;
    const int sr = is71 ? 7 : 5;
    const int bl = 4;   // 7.1 back-left (quad handled above)
    const int br = 5;

    if (srcChannel == centre) return kCentreToStereo;   // centre feeds BOTH
    if (srcChannel == 0 && dstChannel == 0) return kDirectToStereo;
    if (srcChannel == 1 && dstChannel == 1) return kDirectToStereo;
    if (srcChannel == sl && dstChannel == 0) return kSurroundAtten;
    if (srcChannel == sr && dstChannel == 1) return kSurroundAtten;
    if (is71 && srcChannel == bl && dstChannel == 0) return kSurroundAtten;
    if (is71 && srcChannel == br && dstChannel == 1) return kSurroundAtten;
    return 0.0f;   // LFE (index 3) and the far-side surrounds land here
}

void AudioPlayer::ResampleLinear(const int16_t* src, size_t srcFrames,
                                 int channels, int16_t* dst,
                                 size_t dstFrames) {
    if (!src || !dst || channels <= 0 || dstFrames == 0 || srcFrames == 0) {
        return;
    }
    if (srcFrames <= 1 || dstFrames <= 1) {
        for (size_t i = 0; i < dstFrames; ++i) {
            for (int c = 0; c < channels; ++c) {
                dst[i * static_cast<size_t>(channels) + c] = src[c];
            }
        }
        return;
    }
    const double step =
        static_cast<double>(srcFrames - 1) / (dstFrames - 1);
    for (size_t i = 0; i < dstFrames; ++i) {
        const double pos = static_cast<double>(i) * step;
        size_t i0 = static_cast<size_t>(pos);
        if (i0 >= srcFrames - 1) i0 = srcFrames - 2;  // pos at the last frame
        const double frac = pos - static_cast<double>(i0);
        for (int c = 0; c < channels; ++c) {
            const double a =
                src[i0 * static_cast<size_t>(channels) + c];
            const double b =
                src[(i0 + 1) * static_cast<size_t>(channels) + c];
            double v = a + (b - a) * frac;
            if (v < -32768.0) v = -32768.0;
            else if (v > 32767.0) v = 32767.0;
            dst[i * static_cast<size_t>(channels) + c] =
                static_cast<int16_t>(std::lround(v));
        }
    }
}

AudioPlayer::~AudioPlayer() {
    Close();
}

// ─── Open / Close ────────────────────────────────────────────────────────

shared::Result<void> AudioPlayer::Open(std::string_view path) {
    if (m_open.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "audio player already open");
    }

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, std::string(path).c_str(), nullptr, nullptr) != 0) {
        return shared::Result<void>(shared::ErrorCode::FileReadError,
                                    "audio: cannot open input file");
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return shared::Result<void>(shared::ErrorCode::InvalidFormat,
                                    "audio: cannot read stream info");
    }

    int streamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        // No audio track: not an error, caller plays video silently.
        avformat_close_input(&fmtCtx);
        m_open.store(true, std::memory_order_release);
        return shared::Result<void>();
    }

    AVStream* stream = fmtCtx->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return shared::Result<void>(shared::ErrorCode::UnsupportedFormat,
                                    "audio: no decoder for stream codec");
    }
    AVCodecContext* decCtx = avcodec_alloc_context3(codec);
    if (!decCtx) {
        avformat_close_input(&fmtCtx);
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "audio: cannot allocate codec context");
    }
    if (avcodec_parameters_to_context(decCtx, stream->codecpar) < 0 ||
        avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return shared::Result<void>(shared::ErrorCode::InvalidFormat,
                                    "audio: cannot open codec");
    }

    m_fmtCtx = fmtCtx;
    m_decCtx = decCtx;
    m_streamIndex = streamIndex;
    m_codecName = codec->name ? codec->name : "unknown";
    m_frame = av_frame_alloc();
    m_pkt   = av_packet_alloc();
    if (!m_frame || !m_pkt) {
        Close();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "audio: cannot allocate decode buffers");
    }

    m_sampleRate = stream->codecpar->sample_rate > 0 ? stream->codecpar->sample_rate : 48000;
    m_srcChannels = stream->codecpar->ch_layout.nb_channels > 0
                        ? stream->codecpar->ch_layout.nb_channels : 2;
    // The DEVICE layout is always stereo. Requesting the source layout (6ch
    // for AC-3/E-AC-3) in SHARED mode makes the engine period describe a
    // format the endpoint never hands us, so writing framesAvail*m_channels
    // samples overruns the endpoint buffer and faults. Mixing down to stereo
    // here also means the 5.1 downmix matrix is actually used, instead of the
    // surround channels being dropped.
    m_channels = 2;
    m_bytesPerSample = 2;  // we always feed interleaved S16

    // Ring: 1 second of audio, but never less than 4x the WASAPI buffer.
    m_ringFrames = std::max<size_t>(static_cast<size_t>(m_sampleRate),
                                    static_cast<size_t>(m_bufferFrames) * 4);
    m_ring.assign(m_ringFrames * static_cast<size_t>(m_channels), 0);
    m_readPos = m_writePos = m_bufferedFrames = 0;
    m_eof = false;
    m_hasAudio = true;

    m_playing.store(false, std::memory_order_release);
    m_position.store(0.0, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&AudioPlayer::ThreadLoop, this);

    m_open.store(true, std::memory_order_release);
REX_LOG_INFO(kModule, "opened audio stream {} ({} Hz, {} ch -> {} ch device)",
                 streamIndex, m_sampleRate, m_srcChannels, m_channels);
    return shared::Result<void>();
}

void AudioPlayer::Close() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_pkt)   { av_packet_free(&m_pkt); }
    if (m_frame) { av_frame_free(&m_frame); }
    if (m_decCtx) { avcodec_free_context(&m_decCtx); }
    if (m_fmtCtx) { avformat_close_input(&m_fmtCtx); }
    m_streamIndex = -1;
    m_hasAudio = false;
    m_open.store(false, std::memory_order_release);
}

// ─── Control ─────────────────────────────────────────────────────────────

void AudioPlayer::Play() {
    if (!m_hasAudio) return;
    m_playing.store(true, std::memory_order_release);
}

void AudioPlayer::Pause() {
    m_playing.store(false, std::memory_order_release);
}

void AudioPlayer::Stop() {
    m_playing.store(false, std::memory_order_release);
    m_seekTime.store(0.0, std::memory_order_release);
    m_seekRequested.store(true, std::memory_order_release);
}

void AudioPlayer::SetLoop(bool loop) {
    m_looping.store(loop, std::memory_order_relaxed);
}

void AudioPlayer::Seek(double seconds) {
    if (!m_hasAudio) return;
    m_seekTime.store(std::max(0.0, seconds), std::memory_order_release);
    m_seekRequested.store(true, std::memory_order_release);
}

void AudioPlayer::SetVolume(float volume) {
    m_volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

void AudioPlayer::SetSpeed(float speed) {
    m_speed.store(std::clamp(speed, 0.25f, 4.0f), std::memory_order_relaxed);
}

void AudioPlayer::SetMute(bool mute) {
    m_muted.store(mute, std::memory_order_relaxed);
}

// ─── Internal thread ─────────────────────────────────────────────────────

int AudioModeChannels(AudioOutputMode mode) {
    switch (mode) {
        case AudioOutputMode::Auto:      return 0;  // resolved per endpoint
        case AudioOutputMode::Mono:      return 1;
        case AudioOutputMode::Stereo:    return 2;
        case AudioOutputMode::Quad:      return 4;
        case AudioOutputMode::Surround51:return 6;
    }
    return 2;
}

AudioOutputMode AudioChannelsToMode(int channels) {
    switch (channels) {
        case 1: return AudioOutputMode::Mono;
        case 2: return AudioOutputMode::Stereo;
        case 4: return AudioOutputMode::Quad;
        case 6: return AudioOutputMode::Surround51;
        default: return AudioOutputMode::Stereo;
    }
}

const char* AudioModeName(AudioOutputMode mode) {
    switch (mode) {
        case AudioOutputMode::Auto:      return "auto";
        case AudioOutputMode::Mono:      return "mono";
        case AudioOutputMode::Stereo:    return "stereo";
        case AudioOutputMode::Quad:      return "quad";
        case AudioOutputMode::Surround51:return "5.1";
    }
    return "stereo";
}

// The layouts we can request, widest first. Negotiation walks this ladder
// down until it finds one the endpoint can actually render.
static constexpr int kChannelLadder[] = {6, 4, 2, 1};

int AudioPlayer::NegotiateChannels(AudioOutputMode requested, int sourceChannels,
                                   int endpointMixChannels) {
    if (endpointMixChannels < 1) endpointMixChannels = 2;  // unknown: be safe
    const int want = AudioModeChannels(requested) != 0
                         ? AudioModeChannels(requested)
                         : (sourceChannels > 0 ? sourceChannels : 2);
    // Exact match on the ladder?
    for (int ch : kChannelLadder) {
        if (ch == want) return ch <= endpointMixChannels ? ch : 0;
    }
    // Unsupported request (3, 5, 8, ...): widest ladder entry that fits both
    // the request and the endpoint.
    for (int ch : kChannelLadder) {
        if (ch <= want && ch <= endpointMixChannels) return ch;
    }
    return 1;
}

void AudioPlayer::SetOutputMode(AudioOutputMode mode) {
    if (m_requestedMode.exchange(mode, std::memory_order_acq_rel) == mode) return;
    // The audio thread re-negotiates the endpoint and resizes the ring.
    m_modeChanged.store(true, std::memory_order_release);
}

bool AudioPlayer::OpenEndpoint() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (FAILED(hr)) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(hr)) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(m_client.GetAddressOf()));
        }
    }

    if (SUCCEEDED(hr)) {
        // Ask the endpoint what it can actually render BEFORE choosing a
        // layout. Requesting more channels than the mix format provides is
        // what faulted the runtime on 5.1 Dolby tracks: Initialize can
        // succeed, but the engine period still describes the endpoint's real
        // layout, so writing framesAvail*requested channels overruns.
        m_endpointMixChannels = 2;
        WAVEFORMATEXTENSIBLE* mix = nullptr;
        if (SUCCEEDED(m_client->GetMixFormat(
                reinterpret_cast<WAVEFORMATEX**>(&mix))) && mix) {
            const int mixCh = static_cast<int>(
                reinterpret_cast<WAVEFORMATEX*>(mix)->nChannels);
            if (mixCh > 0) m_endpointMixChannels = mixCh;
            CoTaskMemFree(mix);
        }

        const AudioOutputMode requested = m_requestedMode.load(std::memory_order_acquire);
        const int want = NegotiateChannels(requested, m_srcChannels,
                                           m_endpointMixChannels);
        // An exact request that the endpoint cannot honour still resolves to
        // the widest safe layout; flag it so the UI can say so.
        const int requestedChannels = AudioModeChannels(requested);
        const bool exact = requestedChannels != 0 && want == requestedChannels;
        m_modeFallback.store(requestedChannels != 0 && !exact,
                            std::memory_order_release);

        const int rate = m_sampleRate > 0 ? m_sampleRate : 48000;
        // Try the negotiated layout, stepping down the ladder if Initialize
        // refuses it. Mono/stereo are the last resort and always succeed in
        // practice, so the loop always terminates with a usable layout.
        hr = E_FAIL;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const int ch = NegotiateChannels(
                requested, m_srcChannels,
                want < m_endpointMixChannels ? want : m_endpointMixChannels);
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = static_cast<WORD>(ch);
            wf.nSamplesPerSec = static_cast<DWORD>(rate);
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = static_cast<WORD>(ch * 2);
            wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                      0, 0, &wf, nullptr);
            if (SUCCEEDED(hr)) { m_channels = ch; break; }
            if (ch <= 1) break;
            m_endpointMixChannels = want;   // force a narrower retry
        }
        if (SUCCEEDED(hr)) {
            const AudioOutputMode active = AudioChannelsToMode(m_channels);
            m_activeMode.store(active, std::memory_order_release);
            if (m_modeFallback.load(std::memory_order_acquire)) {
                REX_LOG_WARN(kModule,
                             "endpoint supports {} ch; '{}' requested -> using {}",
                             m_endpointMixChannels, AudioModeName(requested),
                             AudioModeName(active));
            } else {
                REX_LOG_INFO(kModule, "endpoint layout negotiated: {}",
                             AudioModeName(active));
            }
            // The ring layout must match what WASAPI will render.
            ResizeRing(m_channels);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = m_client->GetBufferSize(&m_bufferFrames);
    }
    if (SUCCEEDED(hr)) {
        hr = m_client->GetService(IID_PPV_ARGS(&m_render));
    }
    if (SUCCEEDED(hr)) {
REX_LOG_INFO(kModule, "WASAPI render endpoint ready ({} frames/buffer, {} ch)",
                 m_bufferFrames, m_channels);
        return true;
    }
    REX_LOG_WARN(kModule, "WASAPI unavailable (0x{:08X}); audio disabled", hr);
    m_client.Reset();
    m_render.Reset();
    return false;
}

void AudioPlayer::ThreadLoop() {
    // All WASAPI/COM work happens on this thread: init here, release before
    // exit so CoUninitialize always pairs with CoInitializeEx on one thread.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Real-time scheduling for glitch-free rendering: without MMCSS this
    // normal-priority thread is preempted by video decode and misses the
    // ~20 ms buffer deadlines — constant crackle on heavy content.
    // Same pattern as AudioCapture (valid task-index variable; revert
    // on exit — NULL index crashes inside AVRT.dll).
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristics(L"Audio", &mmcssTaskIndex);

    bool wasapiOk = OpenEndpoint();
    auto lastEndpointRetry = std::chrono::steady_clock::now();
    auto lastStats = std::chrono::steady_clock::now();
    uint64_t lastRendered = 0;
    uint64_t decodedAtLastStats = 0;
    constexpr auto kEndpointRetryInterval = std::chrono::seconds(2);
    // The WASAPI stream must be Start()ed or the engine never pulls
    // buffers — everything looks healthy (endpoint ready, unmuted,
    // thread rendering) but nothing reaches the speakers. Owned here
    // (never from Play()/Pause()) so the client has a single owner and
    // re-enumeration can't race a cross-thread Start (cf. AudioCapture,
    // which owns its own client the same way).
    bool streamStarted = false;

    while (m_running.load(std::memory_order_acquire)) {
        // P3-G4: endpoint loss (unplug/default change) re-enumerates instead
        // of silencing audio until reload. Immediate attempt on loss, 2 s
        // backoff while it stays unavailable.
        if (m_endpointLost.exchange(false, std::memory_order_acq_rel)) {
            REX_LOG_WARN(kModule, "render endpoint lost; re-enumerating");
            if (streamStarted && m_client) {
                m_client->Stop();
            }
            streamStarted = false;
            m_render.Reset();
            m_client.Reset();
            wasapiOk = OpenEndpoint();
            lastEndpointRetry = std::chrono::steady_clock::now();
        } else if (!wasapiOk &&
                   std::chrono::steady_clock::now() - lastEndpointRetry >
                       kEndpointRetryInterval) {
            wasapiOk = OpenEndpoint();
            lastEndpointRetry = std::chrono::steady_clock::now();
        }
        if (m_seekRequested.exchange(false, std::memory_order_acq_rel)) {
            DoSeek(m_seekTime.load(std::memory_order_acquire));
        }
        // Output-mode change from the UI: drop the stream and re-negotiate.
        // The ring is resized to whatever the endpoint actually supports.
        if (m_modeChanged.exchange(false, std::memory_order_acq_rel)) {
            if (streamStarted && m_client) {
                m_client->Stop();
                streamStarted = false;
            }
            m_render.Reset();
            m_client.Reset();
            wasapiOk = OpenEndpoint();
            lastEndpointRetry = std::chrono::steady_clock::now();
        }
        if (!m_playing.load(std::memory_order_acquire)) {
            if (streamStarted && m_client) {
                m_client->Stop();
                streamStarted = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (wasapiOk && m_client) {
            // Objective audio health, so "is there sound?" is answerable
            // without listening: real-time rate means frames are actually
            // reaching the endpoint.
            const auto nowStats = std::chrono::steady_clock::now();
            if (nowStats - lastStats >= std::chrono::seconds(10)) {
                const uint64_t rendered = m_renderedFrames.load(std::memory_order_relaxed);
                const uint64_t decoded = m_decodedFrames.load(std::memory_order_relaxed);
                const uint64_t prev = lastRendered;
                lastRendered = rendered;
                lastStats = nowStats;
                const uint64_t dt = 10;
                REX_LOG_INFO(kModule,
                             "audio out: {} fps decoded, {} fps rendered, {} underruns, {}ch -> {}ch @ {} Hz",
                             (decoded - decodedAtLastStats) / dt,
                             (rendered - prev) / dt,
                             m_underruns.load(std::memory_order_relaxed),
                             m_srcChannels, m_channels, m_sampleRate);
                decodedAtLastStats = decoded;
            }
            if (!streamStarted) {
                // Prime the ring BEFORE Start(). Starting an empty ring
                // forces the first buffers to render silence, and any decoder
                // hiccup after that zero-fills a gap — the classic
                // beep-beep at the head of playback. ~150 ms of headroom
                // absorbs a decode hiccup without an audible dropout.
                if (!streamStarted && !PrimedForStart()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;  // bounded: keep trying, never block forever
                }
                const HRESULT startHr = m_client->Start();
                if (SUCCEEDED(startHr)) {
                    streamStarted = true;
                    REX_LOG_INFO(kModule, "WASAPI stream started");
                } else {
                    REX_LOG_WARN(kModule, "IAudioClient::Start failed (0x{:08X}); re-enumerating",
                                 static_cast<unsigned>(startHr));
                    m_endpointLost.store(true, std::memory_order_release);
                    continue;
                }
            }
            RenderStep();
        }
        size_t freeFrames;
        {
            std::lock_guard lock(m_ringMutex);
            freeFrames = m_ringFrames - m_bufferedFrames;
        }
        if (freeFrames >= kMinFreeChunkFrames) {
            if (!DecodeNextChunk()) {
                if (m_looping.load(std::memory_order_acquire)) {
                    if (!RewindToStart()) {
                        m_playing.store(false, std::memory_order_release);
                    }
                } else {
                    m_playing.store(false, std::memory_order_release);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (streamStarted && m_client) {
        m_client->Stop();
        streamStarted = false;
    }
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    m_render.Reset();
    m_client.Reset();
    CoUninitialize();
}

// ─── Demux / decode ──────────────────────────────────────────────────────

// ─── FFmpeg send/receive state machine ───────────────────────────────────

int AudioPlayer::DrainAvailableFrames(AVCodecContext* dec, AVFrame* frame,
                                      const std::function<void(AVFrame*)>& sink) {
    // FFmpeg: "repeat this call until it returns AVERROR(EAGAIN) or an error"
    // — a single packet may yield 0 or more than one frame.
    int delivered = 0;
    for (;;) {
        const int rr = avcodec_receive_frame(dec, frame);
        if (rr == 0) {
            ++delivered;
            sink(frame);
            av_frame_unref(frame);
            continue;
        }
        if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) return delivered;
        return rr;   // terminal error — caller logs it
    }
}

int AudioPlayer::SubmitPacketAndDrain(AVCodecContext* dec, AVPacket* pkt,
                                      AVFrame* frame,
                                      const std::function<void(AVFrame*)>& sink) {
    int delivered = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const int sr = avcodec_send_packet(dec, pkt);
        if (sr == 0) break;                 // accepted
        if (sr == AVERROR(EAGAIN)) {
            // The decoder has not drained its previous output. FFmpeg
            // requires draining and then resubmitting THIS SAME packet —
            // unreffing it here (as the old loop did) silently loses audio.
            const int drained = DrainAvailableFrames(dec, frame, sink);
            if (drained < 0) return drained;
            delivered += drained;
            continue;                       // retry the same packet
        }
        return sr;                          // EINVAL / EOF / real failure
    }
    return delivered + DrainAvailableFrames(dec, frame, sink);
}

bool AudioPlayer::DecodeNextChunk() {
    // Sends packets until at least one decoded frame reaches the ring.
    // Returns false at end of stream (m_eof set) or on a terminal error
    // (logged — playback must never stop silently again).
    auto sink = [this](AVFrame* f) {
        if (f->pts != AV_NOPTS_VALUE) {
            const AVRational tb = m_fmtCtx->streams[m_streamIndex]->time_base;
            m_position.store(static_cast<double>(f->pts) * av_q2d(tb),
                             std::memory_order_release);
        }
        PushFrameToRing(f);
    };

    while (true) {
        if (m_eof) {
            // Drain the decoder's internal delay. AC-3/E-AC-3 hold frames
            // back; without this the tail is lost and looping desyncs.
            const int drained = DrainAvailableFrames(m_decCtx, m_frame, sink);
            if (drained < 0) {
                REX_LOG_ERROR(kModule, "EOF drain failed for {}: {}",
                              m_codecName, AvErrStr(drained));
            }
            return false;
        }

        const int pr = av_read_frame(m_fmtCtx, m_pkt);
        if (pr == AVERROR_EOF) {
            m_eof = true;
            avcodec_send_packet(m_decCtx, nullptr);  // enter draining mode
            continue;
        }
        if (pr < 0) {
            REX_LOG_WARN(kModule, "demux read failed ({}); draining",
                         AvErrStr(pr));
            m_eof = true;
            avcodec_send_packet(m_decCtx, nullptr);
            continue;
        }
        if (m_pkt->stream_index != m_streamIndex) {
            av_packet_unref(m_pkt);
            continue;
        }

        const int delivered = SubmitPacketAndDrain(m_decCtx, m_pkt, m_frame, sink);
        av_packet_unref(m_pkt);   // only safe AFTER the packet was accepted

        if (delivered < 0) {
            REX_LOG_ERROR(kModule, "{} decode failed: {} ({})", m_codecName,
                          AvErrStr(delivered), delivered);
            m_eof = true;
            return false;
        }
        if (delivered > 0) return true;
        // Packet produced no frame yet — feed the next one.
    }
}

void AudioPlayer::ResizeRing(int channels) {
    if (channels <= 0) return;
    std::lock_guard lock(m_ringMutex);
    if (m_channels == channels && !m_ring.empty()) return;
    m_channels = channels;
    m_ringFrames = std::max<size_t>(static_cast<size_t>(m_sampleRate),
                                    static_cast<size_t>(m_bufferFrames) * 4);
    m_ring.assign(m_ringFrames * static_cast<size_t>(m_channels), 0);
    m_scratch.clear();
    m_readPos = m_writePos = m_bufferedFrames = 0;
    m_speedError = 0.0;
}

bool AudioPlayer::PrimedForStart() {
    if (!m_hasAudio) return true;
    const size_t want = static_cast<size_t>(
        std::max(1, m_sampleRate * kPrefillMs / 1000));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPrefillTimeoutMs);
    for (;;) {
        {
            std::lock_guard lock(m_ringMutex);
            if (m_bufferedFrames >= want) return true;
        }
        // EOF with a short clip: start anyway rather than stall forever.
        if (m_eof || !DecodeNextChunk()) {
            std::lock_guard lock(m_ringMutex);
            if (m_bufferedFrames == 0) {
                REX_LOG_INFO(kModule,
                             "prefill skipped for {} (source exhausted)",
                             m_codecName);
            }
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            REX_LOG_WARN(kModule, "prefill timed out after {} ms ({} buffered)",
                         kPrefillTimeoutMs, [&] {
                             std::lock_guard l(m_ringMutex);
                             return m_bufferedFrames;
                         }());
            return true;  // start with whatever we have
        }
    }
}

bool AudioPlayer::RewindToStart() {
    if (avformat_seek_file(m_fmtCtx, m_streamIndex, INT64_MIN, 0, 0,
                           AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    avcodec_flush_buffers(m_decCtx);
    m_eof = false;
    m_position.store(0.0, std::memory_order_release);
    return true;
}

void AudioPlayer::DoSeek(double seconds) {
    if (!m_fmtCtx) return;
    const AVRational tb = m_fmtCtx->streams[m_streamIndex]->time_base;
    const int64_t ts = static_cast<int64_t>(seconds / av_q2d(tb));
    avformat_seek_file(m_fmtCtx, m_streamIndex, INT64_MIN, ts, ts,
                       AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_decCtx);
    m_eof = false;
    std::lock_guard lock(m_ringMutex);
    m_readPos = m_writePos = m_bufferedFrames = 0;
    m_speedError = 0.0;  // P1-13: tempo debt belongs to the old position
    m_position.store(seconds, std::memory_order_release);
}

// ─── Sample conversion (any AVSampleFormat -> interleaved S16) ───────────

// Reads one sample of a decoded frame as float, handling both planar and
// interleaved layouts. `planar` is derived from the frame's format.
static float ReadSample(const AVFrame* frame, bool planar, int ch, int idx,
                        int srcChannels) {
    const uint8_t* plane = frame->data[planar ? ch : 0];
    if (!plane) return 0.0f;
    switch (frame->format) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
            return (static_cast<int>(plane[planar ? idx
                                                 : idx * srcChannels + ch]) - 128)
                   / 128.0f;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            return reinterpret_cast<const int16_t*>(plane)[planar
                       ? idx : idx * srcChannels + ch] / 32768.0f;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            return reinterpret_cast<const int32_t*>(plane)[planar
                       ? idx : idx * srcChannels + ch] / 2147483648.0f;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            return reinterpret_cast<const float*>(plane)[planar
                       ? idx : idx * srcChannels + ch];
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
            return static_cast<float>(reinterpret_cast<const double*>(plane)[planar
                       ? idx : idx * srcChannels + ch]);
        default:
            return 0.0f;
    }
}

static bool IsSupportedSampleFormat(AVSampleFormat f) {
    switch (f) {
        case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_U8P:
        case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P:
        case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P:
        case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP:
        case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP:
            return true;
        default:
            return false;
    }
}

int AudioPlayer::ConvertFrameToS16(const AVFrame* frame, int dstChannels,
                                   int16_t* dst, size_t dstCapacityFrames) {
    if (!frame || !dst || dstChannels <= 0 || dstCapacityFrames == 0) return 0;
    const int frames = frame->nb_samples;
    if (frames <= 0) return 0;
    const int srcChannels = frame->ch_layout.nb_channels > 0
                                ? frame->ch_layout.nb_channels : dstChannels;
    if (srcChannels <= 0) return 0;
    if (!IsSupportedSampleFormat(static_cast<AVSampleFormat>(frame->format))) {
        return AVERROR(ENOSYS);
    }
    const bool planar = av_sample_fmt_is_planar(
        static_cast<AVSampleFormat>(frame->format));
    const size_t n = std::min<size_t>(static_cast<size_t>(frames),
                                      dstCapacityFrames);

    for (size_t i = 0; i < n; ++i) {
        for (int d = 0; d < dstChannels; ++d) {
            float acc = 0.0f;
            for (int c = 0; c < srcChannels; ++c) {
                const float k = GetDownmixCoefficients(srcChannels, dstChannels, c, d);
                if (k == 0.0f) continue;      // channel does not reach this output
                acc += k * ReadSample(frame, planar, c, static_cast<int>(i), srcChannels);
            }
            dst[i * static_cast<size_t>(dstChannels) + d] = ClampToS16(acc);
        }
    }
    return static_cast<int>(n);
}

int AudioPlayer::PushFrameToRing(AVFrame* frame) {
    const int frames = frame->nb_samples;
    // P1-11: the ring layout is ALWAYS the device channel count. Frames
    // carrying more channels (5.1/quad on a stereo ring, mid-stream layout
    // change) are downmixed by dropping the extras — the old
    // max(frameCh, m_channels) stride wrote past the allocation (heap
    // overflow) and corrupted the layout RenderStep reads with m_channels.
    const int srcChannels = frame->ch_layout.nb_channels > 0
                                ? frame->ch_layout.nb_channels : m_channels;
    const int channels = m_channels > 0 ? m_channels : srcChannels;
    if (frames <= 0 || channels <= 0 || srcChannels <= 0) {
        return 0;
    }
    std::lock_guard lock(m_ringMutex);
    if (m_bufferedFrames + static_cast<size_t>(frames) > m_ringFrames) {
        return 0;  // ring full: drop-new (decode is gated, rare)
    }

    // Convert+downmix ONCE into a contiguous scratch buffer, then do a single
    // wrap-aware copy into the ring. Keeping conversion and ring storage apart
    // means the channel matrix and the wrap logic cannot drift into each other.
    const size_t needed = static_cast<size_t>(frames) *
                          static_cast<size_t>(channels);
    if (m_scratch.size() < needed) m_scratch.resize(needed);
    const int converted = ConvertFrameToS16(frame, channels, m_scratch.data(),
                                            static_cast<size_t>(frames));
    if (converted <= 0) {
        if (converted < 0) {
            REX_LOG_WARN(kModule, "unsupported audio sample format {} ({})",
                         static_cast<int>(frame->format), av_get_sample_fmt_name(
                             static_cast<AVSampleFormat>(frame->format)));
        }
        return 0;
    }

    // The ring is CIRCULAR (m_writePos advances modulo m_ringFrames), so the
    // copy must split at the wrap boundary. Indexing it linearly wrote past
    // the end of the vector whenever m_writePos + frames > m_ringFrames —
    // which is exactly what happens once the consumer has advanced m_readPos
    // past 0, i.e. once audio has actually played. That heap overflow is what
    // corrupted the process heap (0xC0000374) and killed the runtime on every
    // switch away from a playing video.
    int16_t* const ringBase = m_ring.data();
    const size_t ringCh = static_cast<size_t>(channels);
    const size_t total = static_cast<size_t>(converted) * ringCh;
    size_t done = 0;
    size_t wp = m_writePos;
    while (done < total) {
        if (wp >= m_ringFrames) wp = 0;
        const size_t avail = (m_ringFrames - wp) * ringCh;
        const size_t n = std::min(total - done, avail);
        // Ring offset is wp*ringCh ONLY — `done` indexes the scratch source,
        // never the ring destination (adding it ran past the ring end).
        std::memcpy(ringBase + wp * ringCh, m_scratch.data() + done,
                    n * sizeof(int16_t));
        done += n;
        wp += n / ringCh;
    }
    m_writePos = (m_writePos + static_cast<size_t>(converted)) % m_ringFrames;
    m_bufferedFrames += static_cast<size_t>(converted);
    m_decodedFrames.fetch_add(static_cast<uint64_t>(converted),
                              std::memory_order_relaxed);
    return converted;
}

// ─── WASAPI render ───────────────────────────────────────────────────────

void AudioPlayer::RenderStep() {
    UINT32 padding = 0;
    // P3-G4: endpoint loss (unplug/default change) flags re-enumeration;
    // transient failures stay silent as before.
    HRESULT hr = m_client->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        if (IsEndpointLost(hr)) {
            REX_LOG_WARN(kModule, "endpoint lost (0x{:08X}); re-enumerating",
                         static_cast<unsigned>(hr));
            m_endpointLost.store(true, std::memory_order_release);
        }
        return;
    }
    if (padding >= m_bufferFrames) {
        return;
    }
    const UINT32 framesAvail = m_bufferFrames - padding;
    const bool muted = m_muted.load(std::memory_order_relaxed);
    const float volume = m_volume.load(std::memory_order_relaxed);

    BYTE* data = nullptr;
    hr = m_render->GetBuffer(framesAvail, &data);
    if (FAILED(hr) || !data) {
        if (FAILED(hr) && IsEndpointLost(hr)) {
            REX_LOG_WARN(kModule, "endpoint lost (0x{:08X}); re-enumerating",
                         static_cast<unsigned>(hr));
            m_endpointLost.store(true, std::memory_order_release);
        }
        return;
    }

    int16_t* dst = reinterpret_cast<int16_t*>(data);
    size_t have = 0;
    {
        std::lock_guard lock(m_ringMutex);
        have = std::min<size_t>(m_bufferedFrames, framesAvail);
        const float speed = m_speed.load(std::memory_order_relaxed);
        if (have > 0 && speed != 1.0f) {
            // P1-13: tempo playback — consume have*speed content frames,
            // resampled to have device frames. Fractional debt carries
            // across callbacks so the long-term rate is exact.
            m_speedError += static_cast<double>(have) * speed;
            size_t contentFrames = static_cast<size_t>(m_speedError);
            m_speedError -= static_cast<double>(contentFrames);
            if (contentFrames > m_bufferedFrames) {
                contentFrames = m_bufferedFrames;
            }
            if (contentFrames == 0) {
                have = 0;  // wait for more content (zero-filled below)
            } else {
                const size_t ch = static_cast<size_t>(m_channels);
                std::vector<int16_t> content(contentFrames * ch);
                const size_t first =
                    std::min(contentFrames, m_ringFrames - m_readPos);
                std::memcpy(content.data(),
                            m_ring.data() + m_readPos * ch, first * ch * 2);
                if (contentFrames > first) {
                    std::memcpy(content.data() + first * ch, m_ring.data(),
                                (contentFrames - first) * ch * 2);
                }
                ResampleLinear(content.data(), contentFrames,
                               m_channels, dst, have);
                m_readPos = (m_readPos + contentFrames) % m_ringFrames;
                m_bufferedFrames -= contentFrames;
            }
        } else if (have > 0) {
            // Split at the ring wrap (see PushFrameToRing): reading
            // m_readPos + have linearly ran off the end of m_ring once the
            // read position wrapped.
            const size_t ringCh = static_cast<size_t>(m_channels);
            const size_t total = have * ringCh;
            size_t done = 0;
            size_t rp = m_readPos;
            while (done < total) {
                const size_t avail = (m_ringFrames - rp) * ringCh;
                const size_t n = std::min(total - done, avail);
                // `done` indexes the (linear) WASAPI destination; the ring
                // source offset is rp*ringCh only.
                std::memcpy(reinterpret_cast<int16_t*>(dst) + done,
                            m_ring.data() + rp * ringCh,
                            n * sizeof(int16_t));
                done += n;
                rp += n / ringCh;
                if (rp >= m_ringFrames) rp = 0;
            }
            m_readPos = (m_readPos + have) % m_ringFrames;
            m_bufferedFrames -= have;
        }
    }
    const size_t totalSamples = framesAvail * static_cast<size_t>(m_channels);
    if (muted || volume <= 0.0f) {
        std::memset(dst, 0, totalSamples * 2);
    } else if (volume < 1.0f) {
        for (size_t i = 0; i < totalSamples; ++i) {
            dst[i] = static_cast<int16_t>(dst[i] * volume);
        }
    }
    // Unconsumed tail (have..framesAvail) was zero-filled above (silence) or
    // left as GetBuffer contents; zero it explicitly for underruns.
    if (have < framesAvail) {
        std::memset(dst + have * static_cast<size_t>(m_channels), 0,
                    (framesAvail - have) * static_cast<size_t>(m_channels) * 2);
    }
    m_render->ReleaseBuffer(framesAvail, 0);
    m_renderedFrames.fetch_add(framesAvail, std::memory_order_relaxed);
    // Ring ran short of the endpoint period: the tail is zero-filled silence,
    // which is exactly the "beep" artefact. Counted so it can be measured
    // without needing ears.
    if (have < framesAvail) {
        m_underruns.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace rex::wallpaper
