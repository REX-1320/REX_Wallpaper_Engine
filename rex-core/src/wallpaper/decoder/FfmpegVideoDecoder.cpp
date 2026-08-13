#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <shared/Logger.hpp>

#include <d3d11_1.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>

namespace rex::wallpaper {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

namespace {

constexpr const char* kModule = "FfmpegVideoDecoder";

std::string Hex(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

FfmpegVideoDecoder::FfmpegVideoDecoder() = default;

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    Close();
}

shared::Result<void> FfmpegVideoDecoder::Open(std::string_view path, bool useHardware,
                                              ID3D11DeviceContext* context11,
                                              HANDLE fenceHandle) {
    if (m_open.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized, "decoder already open");
    }
    if (path.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument, "empty path");
    }
    if (!context11) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "a D3D11 context is required (same adapter as D3D12)");
    }

    // The immediate context is only obtainable from D3D11CreateDevice's
    // out-param (ID3D11Device does NOT QI into the context); recover the
    // device from it instead. ID3D11DeviceChild::GetDevice returns void.
    m_context = context11;
    m_context->GetDevice(&m_device11);
    if (!m_device11) {
        m_context.Reset();
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "cannot get D3D11 device from context");
    }
    ID3D11VideoDevice* rawVideoDevice = nullptr;
    ID3D11VideoContext* rawVideoContext = nullptr;
    ID3D11VideoContext1* rawVideoContext1 = nullptr;
    ID3D11DeviceContext4* rawContext4 = nullptr;
    HRESULT hVid  = m_device11->QueryInterface(IID_PPV_ARGS(&rawVideoDevice));
    HRESULT hVCtx = context11->QueryInterface(IID_PPV_ARGS(&rawVideoContext));
    HRESULT hVC1  = context11->QueryInterface(IID_PPV_ARGS(&rawVideoContext1));
    HRESULT hCtx4 = context11->QueryInterface(IID_PPV_ARGS(&rawContext4));
    if (FAILED(hVid) || FAILED(hVCtx) || FAILED(hVC1) || FAILED(hCtx4)) {
        m_context.Reset();
        m_device11.Reset();
        return shared::Result<void>(
            shared::ErrorCode::HardwareNotSupported,
            "D3D11 device lacks video support (vid=0x" + Hex(hVid) + ", vctx=0x" +
                Hex(hVCtx) + ", vctx1=0x" + Hex(hVC1) + ", ctx4=0x" + Hex(hCtx4) + ")");
    }
    m_videoDevice.Attach(rawVideoDevice);
    m_videoContext.Attach(rawVideoContext);
    m_videoContext1.Attach(rawVideoContext1);
    m_context4.Attach(rawContext4);

    if (fenceHandle) {
        Microsoft::WRL::ComPtr<ID3D11Device5> d5;
        if (FAILED(m_device11.As(&d5)) ||
            FAILED(d5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&m_fence11)))) {
            m_device11.Reset();
            return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                        "failed to open shared fence (OpenSharedFence)");
        }
    } else {
        m_device11.Reset();
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "a shared fence handle is required");
    }

    // ── FFmpeg: open input ──
    av_log_set_level(AV_LOG_ERROR);
    m_fmtCtx = nullptr;
    if (avformat_open_input(&m_fmtCtx, std::string(path).c_str(), nullptr, nullptr) < 0) {
        m_fence11.Reset();
        m_device11.Reset();
        return shared::Result<void>(shared::ErrorCode::FileNotFound, "cannot open input file");
    }
    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        Close();
        return shared::Result<void>(shared::ErrorCode::InvalidFormat, "cannot read stream info");
    }
    m_videoStreamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIndex < 0) {
        Close();
        return shared::Result<void>(shared::ErrorCode::InvalidFormat, "no video stream found");
    }
    m_stream = m_fmtCtx->streams[m_videoStreamIndex];
    m_duration = m_fmtCtx->duration != AV_NOPTS_VALUE
                     ? static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE
                     : 0.0;
    if (m_stream->r_frame_rate.den > 0) {
        m_frameRate = static_cast<double>(m_stream->r_frame_rate.num) /
                      m_stream->r_frame_rate.den;
    }
    if (m_frameRate <= 0.0) {
        m_frameRate = 30.0;
    }
    m_width  = m_stream->codecpar->width;
    m_height = m_stream->codecpar->height;

    const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
    if (!codec) {
        Close();
        return shared::Result<void>(shared::ErrorCode::UnsupportedFormat, "no decoder for codec");
    }
    m_decCtx = avcodec_alloc_context3(codec);
    if (!m_decCtx || avcodec_parameters_to_context(m_decCtx, m_stream->codecpar) < 0) {
        Close();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory, "cannot create codec context");
    }
    m_decCtx->thread_count = 1;  // deterministic sequential decode (wallpaper-style)
    m_decCtx->pkt_timebase = m_stream->time_base;

    // ── Hardware decode (D3D11VA on our D3D11 device) ──
    m_hardware = false;
    if (useHardware) {
        AVBufferRef* hwDevRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (hwDevRef) {
            AVHWDeviceContext* hwDev =
                reinterpret_cast<AVHWDeviceContext*>(hwDevRef->data);
            AVD3D11VADeviceContext* hwctx =
                reinterpret_cast<AVD3D11VADeviceContext*>(hwDev->hwctx);
            hwctx->device = m_device11.Get();
            hwctx->BindFlags = D3D11_BIND_SHADER_RESOURCE;  // FFmpeg adds BIND_DECODER
            hwctx->MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                               D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (av_hwdevice_ctx_init(hwDevRef) >= 0) {
                m_decCtx->hw_device_ctx = av_buffer_ref(hwDevRef);
                m_hwDevCtx = hwDevRef;
                if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
                    REX_LOG_WARN(kModule, "hardware decode open failed, retrying software");
                    avcodec_free_context(&m_decCtx);
                    m_decCtx = nullptr;
                    av_buffer_unref(&m_hwDevCtx);
                    m_hwDevCtx = nullptr;
                } else {
                    m_hardware = true;
                }
            } else {
                av_buffer_unref(&hwDevRef);
            }
        }
        if (!m_hardware) {
            REX_LOG_WARN(kModule, "D3D11VA unavailable, falling back to software decode");
        }
    }

    // ── Software fallback ──
    if (!m_hardware) {
        if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
            Close();
            return shared::Result<void>(shared::ErrorCode::UnsupportedFormat,
                                        "software decoder open failed");
        }
        m_sws = sws_getContext(m_width, m_height,
                               static_cast<AVPixelFormat>(m_stream->codecpar->format),
                               m_width, m_height, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            Close();
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "cannot create swscale context");
        }
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    if (!m_frame || !m_swFrame || !m_pkt) {
        Close();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory, "frame/packet alloc failed");
    }

    m_eof = false;
    m_pktPending = false;
    m_open.store(true, std::memory_order_release);
    REX_LOG_INFO(kModule, "opened '{}' {}x{} @{:.2f} fps ({} decode)", path, m_width,
                 m_height, m_frameRate, m_hardware ? "hardware" : "software");
    return shared::Result<void>();
}

void FfmpegVideoDecoder::Close() {
    m_open.store(false, std::memory_order_release);

    if (m_decCtx) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
    }
    av_buffer_unref(&m_hwDevCtx);
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    av_frame_free(&m_frame);
    av_frame_free(&m_swFrame);
    av_packet_free(&m_pkt);
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }

    m_videoStreamIndex = -1;
    m_stream = nullptr;
    m_pktPending = false;
    m_hardware = false;
    m_eof = false;
    m_duration = 0.0;
    m_frameRate = 30.0;
    m_width = 0;
    m_height = 0;

    for (HANDLE h : m_poolHandles) {
        if (h) CloseHandle(h);
    }
    m_poolHandles.clear();
    m_poolTex.clear();
    m_staging.Reset();
    m_enum.Reset();
    m_processor.Reset();
    m_poolW = 0;
    m_poolH = 0;
    m_poolNext = 0;
    m_poolHdr = false;
    m_hdrContent = false;
    m_fenceValue = 0;
    m_frameIndex = 0;
    m_colorInfo = VideoColorInfo{};
    m_sws16bit = false;

    m_fence11.Reset();
    m_context4.Reset();
    m_videoContext1.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_context.Reset();
    m_device11.Reset();
}

bool FfmpegVideoDecoder::Seek(double seconds) {
    if (!m_open.load(std::memory_order_acquire)) return false;
    return SeekInternal(seconds);
}

bool FfmpegVideoDecoder::Flush() {
    if (!m_decCtx) return false;
    avcodec_flush_buffers(m_decCtx);
    m_eof = false;
    return true;
}

bool FfmpegVideoDecoder::SeekInternal(double seconds) {
    if (!m_fmtCtx || !m_stream) return false;
    const int64_t ts = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE),
                                    AV_TIME_BASE_Q, m_stream->time_base);
    int ret = av_seek_frame(m_fmtCtx, m_videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ret = av_seek_frame(m_fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret >= 0) {
        avcodec_flush_buffers(m_decCtx);
        m_eof = false;
        m_pktPending = false;
        return true;
    }
    REX_LOG_WARN(kModule, "seek to {:.2f}s failed (av_seek_frame {})", seconds, ret);
    return false;
}

void FfmpegVideoDecoder::SelectStreamByResolution(int targetWidth, int targetHeight) {
    if (!m_fmtCtx || !m_open.load(std::memory_order_acquire)) return;

    const int64_t targetPixels =
        static_cast<int64_t>(targetWidth) * static_cast<int64_t>(targetHeight);

    // Pass 1: smallest stream that fits within the target resolution.
    int bestIdx = -1;
    int64_t bestPixels = INT64_MAX;
    for (size_t i = 0; i < m_fmtCtx->nb_streams; ++i) {
        AVStream* s = m_fmtCtx->streams[i];
        if (!s || !s->codecpar || s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        const int64_t pixels =
            static_cast<int64_t>(s->codecpar->width) * s->codecpar->height;
        if (s->codecpar->width <= 0 || s->codecpar->height <= 0) continue;
        if (pixels <= targetPixels && pixels < bestPixels) {
            bestIdx = static_cast<int>(i);
            bestPixels = pixels;
        }
    }
    // Pass 2: nothing fits — take the smallest stream available.
    if (bestIdx == -1) {
        bestPixels = INT64_MAX;
        for (size_t i = 0; i < m_fmtCtx->nb_streams; ++i) {
            AVStream* s = m_fmtCtx->streams[i];
            if (!s || !s->codecpar || s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            const int64_t pixels =
                static_cast<int64_t>(s->codecpar->width) * s->codecpar->height;
            if (s->codecpar->width <= 0 || s->codecpar->height <= 0) continue;
            if (pixels < bestPixels) {
                bestIdx = static_cast<int>(i);
                bestPixels = pixels;
            }
        }
    }
    if (bestIdx < 0 || bestIdx == m_videoStreamIndex) return;

    AVStream* newStream = m_fmtCtx->streams[bestIdx];
    const AVCodec* codec = avcodec_find_decoder(newStream->codecpar->codec_id);
    if (!codec) return;

    // ── Re-open the codec on the new stream ──
    avcodec_free_context(&m_decCtx);
    m_decCtx = avcodec_alloc_context3(codec);
    if (!m_decCtx || avcodec_parameters_to_context(m_decCtx, newStream->codecpar) < 0) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
        REX_LOG_ERROR(kModule, "stream switch failed: cannot create codec context");
        return;
    }
    m_decCtx->thread_count = 1;
    m_decCtx->pkt_timebase = newStream->time_base;

    // Keep hardware decode when it was active (device context must be
    // attached before avcodec_open2); fall back to software on failure.
    bool hwOk = false;
    if (m_hardware && m_hwDevCtx) {
        m_decCtx->hw_device_ctx = av_buffer_ref(m_hwDevCtx);
        if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
            REX_LOG_WARN(kModule, "stream switch: hardware open failed, retrying software");
            avcodec_free_context(&m_decCtx);
            m_decCtx = avcodec_alloc_context3(codec);
            if (!m_decCtx ||
                avcodec_parameters_to_context(m_decCtx, newStream->codecpar) < 0) {
                avcodec_free_context(&m_decCtx);
                m_decCtx = nullptr;
                return;
            }
            m_decCtx->thread_count = 1;
            m_decCtx->pkt_timebase = newStream->time_base;
        } else {
            hwOk = true;
        }
    }
    if (!hwOk && avcodec_open2(m_decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
        REX_LOG_ERROR(kModule, "stream switch failed: cannot open decoder");
        return;
    }

    // Software fallback needs a swscale context sized for the new stream.
    if (!hwOk) {
        sws_freeContext(m_sws);
        m_sws = sws_getContext(newStream->codecpar->width, newStream->codecpar->height,
                               static_cast<AVPixelFormat>(newStream->codecpar->format),
                               newStream->codecpar->width, newStream->codecpar->height,
                               AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            avcodec_free_context(&m_decCtx);
            m_decCtx = nullptr;
            REX_LOG_ERROR(kModule, "stream switch failed: cannot create swscale context");
            return;
        }
    }

    // Adopt the new stream: timebase for pts conversion, stream properties,
    // and fresh decode state. The pool is rebuilt lazily by EnsurePool when
    // the next decoded frame has different dimensions.
    m_videoStreamIndex = bestIdx;
    m_stream = newStream;
    m_hardware = hwOk;
    m_duration = m_fmtCtx->duration != AV_NOPTS_VALUE
                     ? static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE
                     : 0.0;
    if (m_stream->r_frame_rate.den > 0) {
        m_frameRate = static_cast<double>(m_stream->r_frame_rate.num) /
                      m_stream->r_frame_rate.den;
    }
    if (m_frameRate <= 0.0) {
        m_frameRate = 30.0;
    }
    m_width  = newStream->codecpar->width;
    m_height = newStream->codecpar->height;
    m_eof = false;
    m_pktPending = false;
    REX_LOG_INFO(kModule, "stream switched: {}x{} @{:.2f} fps ({} decode)", m_width,
                 m_height, m_frameRate, m_hardware ? "hardware" : "software");
}

// ─── Colorimetry ──────────────────────────────────────────────────────────

VideoColorInfo FfmpegVideoDecoder::ResolveColorInfo() const {
    VideoColorInfo info;
    info.srcWidth  = m_frame ? m_frame->width  : m_width;
    info.srcHeight = m_frame ? m_frame->height : m_height;

    // Fallback chain: frame properties → codec context (derived from the
    // stream at avcodec_open2) → reasonable defaults.
    AVColorSpace spc = m_frame ? m_frame->colorspace : m_decCtx->colorspace;
    AVColorTransferCharacteristic trc = m_frame ? m_frame->color_trc : m_decCtx->color_trc;
    AVColorRange rng = m_frame ? m_frame->color_range : m_decCtx->color_range;
    if (spc == AVCOL_SPC_UNSPECIFIED) spc = m_decCtx->colorspace;
    if (trc == AVCOL_TRC_UNSPECIFIED) trc = m_decCtx->color_trc;
    if (rng == AVCOL_RANGE_UNSPECIFIED) rng = m_decCtx->color_range;

    switch (spc) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            info.yuvMatrix = VideoColorInfo::Matrix::BT601;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            info.yuvMatrix = VideoColorInfo::Matrix::BT2020;
            break;
        default:
            info.yuvMatrix = VideoColorInfo::Matrix::BT709;
            break;
    }

    switch (trc) {
        case AVCOL_TRC_SMPTE2084:
            info.transfer = VideoColorInfo::Transfer::PQ;
            break;
        case AVCOL_TRC_ARIB_STD_B67:
            info.transfer = VideoColorInfo::Transfer::HLG;
            break;
        default:
            info.transfer = VideoColorInfo::Transfer::SDR;
            break;
    }

    info.fullRange = (rng == AVCOL_RANGE_JPEG);

    // HDR static metadata: frame side data wins over stream-level
    // metadata. FFmpeg 7+ removed av_stream_get_side_data and the
    // AVCodecParameters mastering/CLL fields; stream metadata now lives in
    // codecpar->coded_side_data (read via av_packet_side_data_get).
    double maxLum = 0.0, minLum = 0.0, maxCll = 0.0, maxFall = 0.0;
    if (m_frame) {
        if (const auto* sd = av_frame_get_side_data(
                m_frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
            const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
            if (md->has_luminance) {
                maxLum = av_q2d(md->max_luminance);
                minLum = av_q2d(md->min_luminance);
            }
        }
        if (const auto* sd = av_frame_get_side_data(
                m_frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
            const auto* cl = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
            maxCll = cl->MaxCLL;
            maxFall = cl->MaxFALL;
        }
    }
    if (m_stream && m_stream->codecpar) {
        if (maxLum <= 0.0) {
            if (const auto* sd = av_packet_side_data_get(
                    m_stream->codecpar->coded_side_data,
                    m_stream->codecpar->nb_coded_side_data,
                    AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
                const auto* md =
                    reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
                if (md->has_luminance) {
                    maxLum = av_q2d(md->max_luminance);
                    minLum = av_q2d(md->min_luminance);
                }
            }
        }
        if (maxCll <= 0.0) {
            if (const auto* sd = av_packet_side_data_get(
                    m_stream->codecpar->coded_side_data,
                    m_stream->codecpar->nb_coded_side_data,
                    AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
                const auto* cl =
                    reinterpret_cast<const AVContentLightMetadata*>(sd->data);
                maxCll = cl->MaxCLL;
                maxFall = cl->MaxFALL;
            }
        }
    }

    const double defaultPeak = (info.transfer == VideoColorInfo::Transfer::HLG) ? 1000.0 : 1000.0;
    if (info.transfer != VideoColorInfo::Transfer::SDR) {
        info.masteringDisplayMaxLuminance = static_cast<float>(maxLum > 0.0 ? maxLum : defaultPeak);
        info.masteringDisplayMinLuminance = static_cast<float>(minLum);
        info.maxContentLightLevel      = static_cast<float>(maxCll > 0.0 ? maxCll : defaultPeak);
        info.maxFrameAverageLightLevel = static_cast<float>(maxFall > 0.0 ? maxFall : defaultPeak * 0.4f);
    }
    return info;
}

bool FfmpegVideoDecoder::DecodeFrame(VideoFrame& outFrame) {
    if (!m_open.load(std::memory_order_acquire) || !m_decCtx) return false;

    const auto t0 = Clock::now();
    while (true) {
        const int r = avcodec_receive_frame(m_decCtx, m_frame);
        if (r == 0) {
            // Resolve colorimetry of this frame (frame side data may change
            // mid-stream). Drives pool format, VP color spaces and the
            // renderer's shader constants.
            m_colorInfo = ResolveColorInfo();
            m_hdrContent = m_colorInfo.transfer != VideoColorInfo::Transfer::SDR;
            outFrame.colorInfo = m_colorInfo;
            const bool ok = ConvertAndSignal(outFrame);
            const double decodeMs = ms(Clock::now() - t0).count();
            m_lastDecodeMs.store(decodeMs, std::memory_order_relaxed);
            m_decodeMsSum.fetch_add(decodeMs, std::memory_order_relaxed);
            if (ok) {
                m_framesDecoded.fetch_add(1, std::memory_order_relaxed);
            }
            return ok;
        }
        if (r == AVERROR(EAGAIN)) {
            // decoder needs more packets; fall through to read one
        } else if (r == AVERROR_EOF) {
            if (m_eof) {
                if (m_looping.load(std::memory_order_relaxed) && SeekInternal(0.0)) {
                    continue;
                }
                return false;
            }
            m_eof = true;
            avcodec_send_packet(m_decCtx, nullptr);  // flush remaining frames
            continue;
        } else {
            REX_LOG_ERROR(kModule, "avcodec_receive_frame failed ({})", r);
            return false;
        }

        if (!m_pktPending) {
            const int pr = av_read_frame(m_fmtCtx, m_pkt);
            if (pr == AVERROR_EOF) {
                if (!m_eof) {
                    m_eof = true;
                    avcodec_send_packet(m_decCtx, nullptr);  // flush remaining frames
                }
                continue;
            }
            if (pr < 0) {
                REX_LOG_ERROR(kModule, "av_read_frame failed ({})", pr);
                return false;
            }
            if (m_pkt->stream_index != m_videoStreamIndex) {
                av_packet_unref(m_pkt);
                continue;
            }
            m_pktPending = true;
        }
        const int s = avcodec_send_packet(m_decCtx, m_pkt);
        av_packet_unref(m_pkt);
        m_pktPending = false;
        if (s == 0 || s == AVERROR(EAGAIN)) {
            continue;
        }
        REX_LOG_ERROR(kModule, "avcodec_send_packet failed ({})", s);
        return false;
    }
}

HANDLE FfmpegVideoDecoder::GetSlotHandle(UINT slot) const {
    if (!m_open.load(std::memory_order_acquire) || slot >= m_poolHandles.size()) {
        return nullptr;
    }
    return m_poolHandles[slot];
}

UINT64 FfmpegVideoDecoder::GetSlotFence(UINT slot) const {
    if (slot >= kPoolSize) return 0;
    return m_slotFence[slot].load(std::memory_order_acquire);
}

DecoderDiagnostics FfmpegVideoDecoder::GetDiagnostics() const {
    DecoderDiagnostics d;
    d.framesDecoded = m_framesDecoded.load(std::memory_order_relaxed);
    d.cpuToGpuCopies = m_cpuToGpuCopies.load(std::memory_order_relaxed);
    d.lastDecodeMs = m_lastDecodeMs.load(std::memory_order_relaxed);
    const double sum = m_decodeMsSum.load(std::memory_order_relaxed);
    const uint64_t n = d.framesDecoded;
    d.averageDecodeMs = n > 0 ? sum / static_cast<double>(n) : 0.0;
    d.hardwareAccelerated = m_hardware;
    d.eof = m_eof;
    return d;
}

bool FfmpegVideoDecoder::EnsurePool(UINT width, UINT height) {
    if (m_poolW == width && m_poolH == height &&
        m_poolTex.size() == kPoolSize && m_processor &&
        m_poolHdr == m_hdrContent) {
        return true;
    }

    for (HANDLE h : m_poolHandles) {
        if (h) CloseHandle(h);
    }
    m_poolHandles.clear();
    m_poolTex.clear();
    m_poolTex.resize(kPoolSize);
    m_poolHandles.resize(kPoolSize, nullptr);
    m_poolW = width;
    m_poolH = height;
    m_poolNext = 0;
    m_poolHdr = m_hdrContent;

    // HDR sources keep the full PQ/HLG-encoded signal in a 16-bit float
    // pool; SDR stays in 8-bit BGRA (matches the renderer's SRV format
    // selection, which reads the opened resource's desc).
    const DXGI_FORMAT poolFormat =
        m_hdrContent ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;

    for (size_t i = 0; i < m_poolTex.size(); ++i) {
        D3D11_TEXTURE2D_DESC pd = {};
        pd.Width = width;
        pd.Height = height;
        pd.MipLevels = 1;
        pd.ArraySize = 1;
        pd.Format = poolFormat;
        pd.SampleDesc.Count = 1;
        pd.Usage = D3D11_USAGE_DEFAULT;
        pd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        pd.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(m_device11->CreateTexture2D(&pd, nullptr, &m_poolTex[i]))) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> res1;
        if (SUCCEEDED(m_poolTex[i].As(&res1))) {
            res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                     &m_poolHandles[i]);
        }
    }

    // Hardware YUV->RGB video processor (NV12 decode -> BGRA pool).
    m_enum.Reset();
    m_processor.Reset();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = { 30, 1 };
    cd.InputWidth = width;
    cd.InputHeight = height;
    cd.OutputFrameRate = { 30, 1 };
    cd.OutputWidth = width;
    cd.OutputHeight = height;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&cd, &m_enum);
    if (SUCCEEDED(hr)) {
        hr = m_videoDevice->CreateVideoProcessor(m_enum.Get(), 0, &m_processor);
    }
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "create video processor failed (0x{:08x})", hr);
        return false;
    }

    // Software-upload staging texture.
    m_staging.Reset();
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = poolFormat;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device11->CreateTexture2D(&sd, nullptr, &m_staging))) {
        REX_LOG_ERROR(kModule, "create staging texture failed");
        return false;
    }
    return true;
}

bool FfmpegVideoDecoder::ConvertAndSignal(VideoFrame& outFrame) {
    if (m_frame->format != AV_PIX_FMT_D3D11) {
        return ConvertSoftware(outFrame);
    }

    // Zero-copy conversion: decoder NV12 texture -> VP blt -> BGRA pool slot.
    auto* tex11 = reinterpret_cast<ID3D11Texture2D*>(m_frame->data[0]);
    const UINT arraySlice = static_cast<UINT>(
        reinterpret_cast<intptr_t>(m_frame->data[1]));
    D3D11_TEXTURE2D_DESC desc11 = {};
    tex11->GetDesc(&desc11);

    if (!EnsurePool(desc11.Width, desc11.Height)) {
        return false;
    }

    const UINT slot = m_poolNext;
    m_poolNext = (m_poolNext + 1) % kPoolSize;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = arraySlice;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(m_videoDevice->CreateVideoProcessorInputView(
            tex11, m_enum.Get(), &ivd, &inView))) {
        REX_LOG_ERROR(kModule, "create vp input view failed");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(m_videoDevice->CreateVideoProcessorOutputView(
            m_poolTex[slot].Get(), m_enum.Get(), &ovd, &outView))) {
        REX_LOG_ERROR(kModule, "create vp output view failed");
        return false;
    }

    RECT srcRect = { 0, 0, static_cast<LONG>(m_frame->width),
                     static_cast<LONG>(m_frame->height) };
    m_videoContext->VideoProcessorSetStreamSourceRect(m_processor.Get(), 0, TRUE,
                                                      &srcRect);

    // Resolve DXGI color spaces from the frame colorimetry (YUV in → RGB
    // out). HDR signals stay in their encoded domain (PQ/HLG) so the 16F
    // pool preserves full precision; the renderer decodes the EOTF.
    auto inputSpace = [&]() {
        switch (m_colorInfo.yuvMatrix) {
            case VideoColorInfo::Matrix::BT601:
                return m_colorInfo.fullRange
                           ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                           : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
            case VideoColorInfo::Matrix::BT2020:
                if (m_colorInfo.transfer == VideoColorInfo::Transfer::PQ)
                    return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
                if (m_colorInfo.transfer == VideoColorInfo::Transfer::HLG)
                    return DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
                return m_colorInfo.fullRange
                           ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                           : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
            default:
                return m_colorInfo.fullRange
                           ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                           : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        }
    }();
    auto outputSpace = [&]() {
        // HDR output is always PQ-encoded BT.2020 in the 16F pool (this
        // SDK has no RGB HLG output space; the video processor converts
        // HLG input to PQ). The shader therefore always decodes PQ.
        if (m_colorInfo.transfer == VideoColorInfo::Transfer::PQ ||
            m_colorInfo.transfer == VideoColorInfo::Transfer::HLG)
            return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }();

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = inView.Get();
    HRESULT bltHr = E_FAIL;
    for (int attempt = 0; attempt < 2 && FAILED(bltHr); ++attempt) {
        m_videoContext1->VideoProcessorSetStreamColorSpace1(
            m_processor.Get(), 0, inputSpace);
        m_videoContext1->VideoProcessorSetOutputColorSpace1(
            m_processor.Get(), outputSpace);
        bltHr = m_videoContext->VideoProcessorBlt(m_processor.Get(), outView.Get(),
                                                  0, 1, &stream);
        if (FAILED(bltHr) && attempt == 0) {
            // Driver may not support the HDR stream/output spaces; retry
            // once as SDR (G22/P709) so playback survives.
            REX_LOG_WARN(kModule,
                         "VP blt failed with HDR color spaces (0x{:08x}), "
                         "retrying as SDR",
                         static_cast<unsigned long>(bltHr));
            inputSpace = m_colorInfo.fullRange
                             ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                             : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
            outputSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        }
    }
    if (FAILED(bltHr)) {
        REX_LOG_ERROR(kModule, "videoprocessor blt failed");
        return false;
    }

    ++m_fenceValue;
    m_slotFence[slot].store(m_fenceValue, std::memory_order_release);
    m_context4->Signal(m_fence11.Get(), m_fenceValue);
    m_context->Flush();

    outFrame.sharedHandle = m_poolHandles[slot];
    outFrame.slot = slot;
    outFrame.fenceValue = m_fenceValue;
    outFrame.pts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                       ? static_cast<double>(av_rescale_q(m_frame->best_effort_timestamp,
                                                          m_stream->time_base,
                                                          AV_TIME_BASE_Q)) / AV_TIME_BASE
                       : 0.0;
    outFrame.frameIndex = m_frameIndex++;
    outFrame.width = m_frame->width;
    outFrame.height = m_frame->height;
    outFrame.valid = true;
    return true;
}

bool FfmpegVideoDecoder::ConvertSoftware(VideoFrame& outFrame) {
    // Software fallback: swscale to BGRA (SDR) / RGBA64 (HDR), staging
    // upload, CopyResource into the pool slot (CPU round-trip, used only
    // when D3D11VA is unavailable).
    if (!m_loggedSwFallback) {
        m_loggedSwFallback = true;
        REX_LOG_WARN(kModule, "software frame conversion (CPU upload) active");
    }
    if (!m_swFrame || !m_stream) return false;
    const auto frameFormat = static_cast<AVPixelFormat>(m_frame->format);
    const AVPixelFormat swFormat = m_hdrContent ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_BGRA;
    if (!m_sws ||
        frameFormat != static_cast<AVPixelFormat>(m_stream->codecpar->format) ||
        m_sws16bit != m_hdrContent) {
        if (m_sws) {
            sws_freeContext(m_sws);
        }
        m_sws = sws_getContext(m_frame->width, m_frame->height, frameFormat,
                               m_frame->width, m_frame->height, swFormat,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            REX_LOG_ERROR(kModule, "swscale context creation failed");
            return false;
        }
        m_sws16bit = m_hdrContent;
        // Tell swscale the source YUV range/matrix so limited/full and
        // 601/709/2020 sources convert correctly.
        const int srcRange = m_colorInfo.fullRange ? 1 : 0;
        const int srcCsp = (m_colorInfo.yuvMatrix == VideoColorInfo::Matrix::BT601)
                               ? SWS_CS_SMPTE170M
                               : (m_colorInfo.yuvMatrix == VideoColorInfo::Matrix::BT2020)
                                     ? SWS_CS_BT2020
                                     : SWS_CS_ITU709;
        sws_setColorspaceDetails(m_sws, sws_getCoefficients(srcCsp), srcRange,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 0,
                                 0, 1 << 16, 1 << 16);
    }

    if (!EnsurePool(static_cast<UINT>(m_frame->width),
                    static_cast<UINT>(m_frame->height))) {
        return false;
    }
    const UINT slot = m_poolNext;
    m_poolNext = (m_poolNext + 1) % kPoolSize;

    av_frame_unref(m_swFrame);
    m_swFrame->format = swFormat;
    m_swFrame->width = m_frame->width;
    m_swFrame->height = m_frame->height;
    if (av_frame_get_buffer(m_swFrame, 32) < 0) {
        return false;
    }
    sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_frame->height,
              m_swFrame->data, m_swFrame->linesize);

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_WRITE, 0, &map))) {
        return false;
    }
    const uint8_t* src = m_swFrame->data[0];
    const int srcPitch = m_swFrame->linesize[0];
    const UINT rowBytes =
        static_cast<UINT>(m_frame->width) * (m_hdrContent ? 8u : 4u);
    uint8_t* dst = static_cast<uint8_t*>(map.pData);
    for (UINT y = 0; y < static_cast<UINT>(m_frame->height); ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch,
                    src + static_cast<size_t>(y) * srcPitch, rowBytes);
    }
    m_context->Unmap(m_staging.Get(), 0);
    m_context->CopyResource(m_poolTex[slot].Get(), m_staging.Get());

    ++m_fenceValue;
    m_slotFence[slot].store(m_fenceValue, std::memory_order_release);
    m_context4->Signal(m_fence11.Get(), m_fenceValue);
    m_context->Flush();

    m_cpuToGpuCopies.fetch_add(1, std::memory_order_relaxed);

    outFrame.sharedHandle = m_poolHandles[slot];
    outFrame.slot = slot;
    outFrame.fenceValue = m_fenceValue;
    outFrame.pts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                       ? static_cast<double>(av_rescale_q(m_frame->best_effort_timestamp,
                                                          m_stream->time_base,
                                                          AV_TIME_BASE_Q)) / AV_TIME_BASE
                       : 0.0;
    outFrame.frameIndex = m_frameIndex++;
    outFrame.width = m_frame->width;
    outFrame.height = m_frame->height;
    outFrame.valid = true;
    return true;
}

} // namespace rex::wallpaper
