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

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "AudioPlayer";
constexpr size_t kMinFreeChunkFrames = 2048;  // pace decode when ring is this full
constexpr size_t kRingSeconds = 1;            // ring capacity in seconds of audio

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
} // namespace

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
    m_frame = av_frame_alloc();
    m_pkt   = av_packet_alloc();
    if (!m_frame || !m_pkt) {
        Close();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "audio: cannot allocate decode buffers");
    }

    m_sampleRate = stream->codecpar->sample_rate > 0 ? stream->codecpar->sample_rate : 48000;
    m_channels   = stream->codecpar->ch_layout.nb_channels > 0
                       ? stream->codecpar->ch_layout.nb_channels : 2;
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
    REX_LOG_INFO(kModule, "opened audio stream {} ({} Hz, {} ch)", streamIndex,
                 m_sampleRate, m_channels);
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
        // Feed the engine's native layout as interleaved S16; AUTOCONVERTPCM
        // makes any rate/channel count acceptable (engine resamples).
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = static_cast<WORD>(m_channels);
        wf.nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = static_cast<WORD>(m_channels * 2);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                  0, 0, &wf, nullptr);
        if (FAILED(hr)) {
            // Fallback: common denominator 48 kHz stereo.
            wf.nChannels = 2;
            wf.nSamplesPerSec = 48000;
            wf.nBlockAlign = 4;
            wf.nAvgBytesPerSec = 48000 * 4;
            hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                      0, 0, &wf, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = m_client->GetBufferSize(&m_bufferFrames);
    }
    if (SUCCEEDED(hr)) {
        hr = m_client->GetService(IID_PPV_ARGS(&m_render));
    }
    if (SUCCEEDED(hr)) {
        REX_LOG_INFO(kModule, "WASAPI render endpoint ready ({} frames/buffer)",
                     m_bufferFrames);
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

    bool wasapiOk = OpenEndpoint();
    auto lastEndpointRetry = std::chrono::steady_clock::now();
    constexpr auto kEndpointRetryInterval = std::chrono::seconds(2);

    while (m_running.load(std::memory_order_acquire)) {
        // P3-G4: endpoint loss (unplug/default change) re-enumerates instead
        // of silencing audio until reload. Immediate attempt on loss, 2 s
        // backoff while it stays unavailable.
        if (m_endpointLost.exchange(false, std::memory_order_acq_rel)) {
            REX_LOG_WARN(kModule, "render endpoint lost; re-enumerating");
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
        if (!m_playing.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (wasapiOk) {
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

    m_render.Reset();
    m_client.Reset();
    CoUninitialize();
}

// ─── Demux / decode ──────────────────────────────────────────────────────

bool AudioPlayer::DecodeNextChunk() {
    while (true) {
        const int pr = av_read_frame(m_fmtCtx, m_pkt);
        if (pr == AVERROR_EOF) {
            m_eof = true;
            return false;
        }
        if (pr < 0) {
            m_eof = true;
            return false;
        }
        if (m_pkt->stream_index != m_streamIndex) {
            av_packet_unref(m_pkt);
            continue;
        }
        avcodec_send_packet(m_decCtx, m_pkt);
        av_packet_unref(m_pkt);

        const int rr = avcodec_receive_frame(m_decCtx, m_frame);
        if (rr == AVERROR(EAGAIN)) {
            continue;  // need more packets
        }
        if (rr < 0) {
            return false;
        }
        if (m_frame->pts != AV_NOPTS_VALUE) {
            const AVRational tb = m_fmtCtx->streams[m_streamIndex]->time_base;
            m_position.store(static_cast<double>(m_frame->pts) * av_q2d(tb),
                             std::memory_order_release);
        }
        PushFrameToRing(m_frame);
        av_frame_unref(m_frame);
        return true;
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
    {
        std::lock_guard lock(m_ringMutex);
        if (m_bufferedFrames + static_cast<size_t>(frames) > m_ringFrames) {
            return 0;  // ring full: drop-new (decode is gated, rare)
        }
        int16_t* dst = m_ring.data() + m_writePos * static_cast<size_t>(channels);
        const bool planar = av_sample_fmt_is_planar(
            static_cast<AVSampleFormat>(frame->format));

        if (frame->format == AV_SAMPLE_FMT_S16 &&
            srcChannels == channels && !planar) {
            const int16_t* src = reinterpret_cast<const int16_t*>(frame->data[0]);
            std::memcpy(dst, src, static_cast<size_t>(frames) * channels * 2);
        } else {
            for (int f = 0; f < frames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    const int srcC = c < srcChannels ? c : 0;
                    float v = 0.0f;
                    const int16_t* p16;
                    const int32_t* p32;
                    const float* pf;
                    const double* pd;
                    switch (frame->format) {
                        case AV_SAMPLE_FMT_U8:
                        case AV_SAMPLE_FMT_U8P:
                            v = (static_cast<int>(
                                    static_cast<const uint8_t*>(frame->data[planar ? srcC : 0])
                                        [planar ? f : f * srcChannels + srcC]) - 128) / 128.0f;
                            break;
                        case AV_SAMPLE_FMT_S16:
                        case AV_SAMPLE_FMT_S16P:
                            p16 = reinterpret_cast<const int16_t*>(
                                frame->data[planar ? srcC : 0]);
                            v = p16[planar ? f : f * srcChannels + srcC] / 32768.0f;
                            break;
                        case AV_SAMPLE_FMT_S32:
                        case AV_SAMPLE_FMT_S32P:
                            p32 = reinterpret_cast<const int32_t*>(
                                frame->data[planar ? srcC : 0]);
                            v = p32[planar ? f : f * srcChannels + srcC] / 2147483648.0f;
                            break;
                        case AV_SAMPLE_FMT_FLT:
                        case AV_SAMPLE_FMT_FLTP:
                            pf = reinterpret_cast<const float*>(
                                frame->data[planar ? srcC : 0]);
                            v = pf[planar ? f : f * srcChannels + srcC];
                            break;
                        case AV_SAMPLE_FMT_DBL:
                        case AV_SAMPLE_FMT_DBLP:
                            pd = reinterpret_cast<const double*>(
                                frame->data[planar ? srcC : 0]);
                            v = static_cast<float>(pd[planar ? f : f * srcChannels + srcC]);
                            break;
                        default:
                            return 0;
                    }
                    dst[f * static_cast<size_t>(channels) + c] = ClampToS16(v);
                }
            }
        }
        m_writePos = (m_writePos + static_cast<size_t>(frames)) % m_ringFrames;
        m_bufferedFrames += static_cast<size_t>(frames);
    }
    return frames;
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
            const size_t samples = have * static_cast<size_t>(m_channels);
            std::memcpy(dst, m_ring.data() + m_readPos * static_cast<size_t>(m_channels),
                        samples * 2);
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
}

} // namespace rex::wallpaper
