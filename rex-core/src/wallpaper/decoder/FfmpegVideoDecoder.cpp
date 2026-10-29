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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <vector>

namespace rex::wallpaper {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

namespace {

constexpr const char* kModule = "FfmpegVideoDecoder";

// ─── Slot leasing (Phase D prerequisite) ───────────────────────────────────
//
// The decode thread must not overwrite a pool slot the render thread is still
// reading. DXGI-style fences give ORDERING (a consumer waiting on fenceValue N
// is ordered after the producer's signal N) but not LIFETIME: once the producer
// overwrites the slot and signals N+1, a consumer that already waited on N sees
// its wait satisfied and samples the NEW pixels while still believing it holds
// frame N.
//
// That was latent while decode paced at the source rate (the renderer consumed
// a slot within a frame or two). Decode-ahead widens that window a lot, so the
// allocator below prefers the slot furthest behind the consumed watermark.
//
// This is pure integer bookkeeping with no device dependency: it is a public
// static (declared in the header) and the lease tests call this exact
// function — no mirror copy (audit 2026-10-17, §5.2 #15).

// Bounded retry budget for avcodec_send_packet returning EAGAIN. Zero means
// never stall; this many retries lets a transiently-full decoder drain first.
constexpr int kMaxSendEagainRetries = 8;

// Attempts without a single decoded frame before REX reports an unusable
// decode path instead of stalling silently.
constexpr uint32_t kZeroProgressReportAfter = 240;

// Consecutive avcodec_send_packet failures before the error is escalated to a
// single actionable message. Below this we still log each failure (a healthy
// stream should not get here); at/above it the decoder is unusable and one
// clear line beats hundreds of identical ones.
constexpr uint32_t kSendFailReportAfter = 8;

std::string Hex(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

FfmpegVideoDecoder::FfmpegVideoDecoder() = default;

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    Close();
    // Close() deliberately keeps the pool generation alive for reuse; the
    // destructor is the real teardown boundary (VideoManager shutdown).
    ReleasePool();
}

// P1-9: polled by FFmpeg inside every blocking demuxer call on this
// decoder's format context. Runs on FFmpeg's IO thread — reads only the
// atomic flag, never takes a lock.
int FfmpegVideoDecoder::InterruptCallback(void* opaque) {
    const auto* self = static_cast<const FfmpegVideoDecoder*>(opaque);
    if (!self) return 0;
    return self->m_stopRequested.load(std::memory_order_acquire) ? 1 : 0;
}

void FfmpegVideoDecoder::RequestStop() {
    m_stopRequested.store(true, std::memory_order_release);
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
    // Audit 2026-10-17, §5.2 #6: QI into ComPtr locals and publish only when
    // all four succeed. The old raw-pointer form leaked every already-
    // AddRef'd interface when a later QI failed (the early return skipped
    // all four Attach calls).
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>   videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>  videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext1;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
    HRESULT hVid  = m_device11.As(&videoDevice);
    HRESULT hVCtx = context11->QueryInterface(
        IID_PPV_ARGS(videoContext.GetAddressOf()));
    HRESULT hVC1  = context11->QueryInterface(
        IID_PPV_ARGS(videoContext1.GetAddressOf()));
    HRESULT hCtx4 = context11->QueryInterface(
        IID_PPV_ARGS(context4.GetAddressOf()));
    if (FAILED(hVid) || FAILED(hVCtx) || FAILED(hVC1) || FAILED(hCtx4)) {
        m_context.Reset();
        m_device11.Reset();
        return shared::Result<void>(
            shared::ErrorCode::HardwareNotSupported,
            "D3D11 device lacks video support (vid=0x" + Hex(hVid) + ", vctx=0x" +
                Hex(hVCtx) + ", vctx1=0x" + Hex(hVC1) + ", ctx4=0x" + Hex(hCtx4) + ")");
    }
    m_videoDevice   = std::move(videoDevice);
    m_videoContext  = std::move(videoContext);
    m_videoContext1 = std::move(videoContext1);
    m_context4      = std::move(context4);

    // Capability probe (Phase 6): ask the driver what it can actually decode
    // instead of discovering the answer by attempting an open and reading a
    // warning afterwards. GetVideoDecoderProfileCount needs no codec GUID, so
    // this is exact where a per-codec query would depend on GUID constants we
    // do not carry. 0 => the device advertises no hardware decode at all.
    m_hwDecoderProfiles = m_videoDevice->GetVideoDecoderProfileCount();
    REX_LOG_INFO(kModule, "hardware decoder profiles reported by driver: {}",
                 m_hwDecoderProfiles);

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

    // â”€â”€ FFmpeg: open input â”€â”€
    av_log_set_level(AV_LOG_ERROR);
    // P1-9: preallocate so the interrupt callback is installed BEFORE
    // open_input — a dead share then fails fast instead of hanging a later
    // av_read_frame until the FFmpeg timeout. (open_input frees a
    // preallocated context on failure and nulls it.)
    m_stopRequested.store(false, std::memory_order_release);
    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        m_fence11.Reset();
        m_device11.Reset();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "cannot allocate format context");
    }
    m_fmtCtx->interrupt_callback.callback = &FfmpegVideoDecoder::InterruptCallback;
    m_fmtCtx->interrupt_callback.opaque = this;
    if (avformat_open_input(&m_fmtCtx, std::string(path).c_str(), nullptr, nullptr) < 0) {
        m_fmtCtx = nullptr;
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
    m_startTime = m_fmtCtx->start_time != AV_NOPTS_VALUE
                      ? static_cast<double>(m_fmtCtx->start_time) / AV_TIME_BASE
                      : 0.0;
    if (m_stream->r_frame_rate.den > 0) {
        m_frameRate = static_cast<double>(m_stream->r_frame_rate.num) /
                      m_stream->r_frame_rate.den;
    }
    {
        // P0-1: trust measured timestamps over the container-declared rate
        // (see ResolveFrameRate: a 60 fps declaration over 25.69 fps
        // timestamps paces decode 2.3x too fast and no scheduler can play
        // the resulting future-filled ring correctly).
        bool usedCount = false;
        const double declared = m_frameRate;
        m_frameRate = ResolveFrameRate(m_stream->nb_frames, m_duration,
                                       declared, &usedCount);
        if (usedCount &&
            (declared <= 0.0 || declared / m_frameRate < 0.95 ||
             declared / m_frameRate > 1.05)) {
            REX_LOG_WARN(kModule,
                         "frame rate {:.2f} fps from {} frames / {:.2f} s, "
                         "not the declared {:.2f} fps",
                         m_frameRate, m_stream->nb_frames, m_duration,
                         declared);
        }
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
// Software decode defaults to FFmpeg's own thread count (0 => auto, which on a
    // 12-core machine expanded to 13). Benchmarked 2026-11 on the shipped
    // FFmpeg, H.264 3840x2160 @30, 300 frames:
    //     thread_count=1 -> 114.7 fps (8.72 ms/frame)
    //     thread_count=0 -> 370.1 fps (2.70 ms/frame)   3.23x
    // Single-threaded leaves only 1.9x headroom at 4K60 and falls BELOW
    // realtime at 4K120, so any CPU contention (and the app runs alongside
    // Chromium) turns it into stutter. The hardware path overrides this to 1
    // below, matching LAV's hwaccel rule. See test_sw_decode_benchmark.cpp,
    // which re-measures this per machine rather than trusting these numbers.
    m_decCtx->thread_count = 0;
    m_decCtx->pkt_timebase = m_stream->time_base;

    // ─── Hardware decode (D3D11VA on our D3D11 device) ───
    if (useHardware && m_hwDecoderProfiles == 0) {
        REX_LOG_INFO(kModule, "driver reports 0 hardware decoder profiles "
                     "— skipping hardware decode, using software path");
    }
    m_hardware = false;
    // Step 2: skip a doomed hardware attempt when the driver exposes no
    // profile for this codec instead of discovering it from avcodec_open2's
    // failure. Unknown codecs always attempt (status quo).
    bool tryHardware = useHardware && m_hwDecoderProfiles > 0;
    // Step 2b: D3D11VA only outputs 4:2:0 (NV12/P010). A stream with any
    // other chroma layout — e.g. HEVC Rext 4:4:4 — opens with hwaccel
    // attached and only fails at get_format, where FFmpeg silently falls
    // back to software decode WHILE KEEPING the hardware thread pin set
    // below (thread_count=1): measured 92-121 ms/frame single-threaded on
    // a 3840x1752 Rext clip vs 21 ms/frame with auto threads (2026-10-17).
    // Skip the doomed attempt so the software path keeps auto threads.
    if (tryHardware &&
        !D3d11vaCanDecodeChroma(
            static_cast<AVPixelFormat>(m_stream->codecpar->format))) {
        const char* fmtName = av_get_pix_fmt_name(
            static_cast<AVPixelFormat>(m_stream->codecpar->format));
        REX_LOG_INFO(kModule,
                     "hardware decode skipped: {} chroma unsupported by "
                     "D3D11VA (4:2:0 only), using software decode with "
                     "auto threads",
                     fmtName ? fmtName : "unknown");
        tryHardware = false;
    }
    if (tryHardware) {
        constexpr UINT kMaxProfiles = 64;
        GUID profiles[kMaxProfiles];
        UINT n = m_hwDecoderProfiles < kMaxProfiles ? m_hwDecoderProfiles
                                                    : kMaxProfiles;
        for (UINT i = 0; i < n; ++i) {
            if (FAILED(m_videoDevice->GetVideoDecoderProfile(i,
                                                             &profiles[i]))) {
                n = i;
                break;
            }
        }
        if (CheckHwProfileSupport(m_stream->codecpar->codec_id, profiles,
                                  n) == HwProfileSupport::Unsupported) {
            // Log what the driver claims (string form) so the next table
            // mismatch is diagnosable from runtime.log alone — a wrong
            // constant here silently forces software decode with different
            // colors (measured 2026-10-16).
            std::string claimed;
            const UINT show = n < 8 ? n : 8;
            for (UINT i = 0; i < show; ++i) {
                char g[64];
                std::snprintf(g, sizeof(g),
                              "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                              static_cast<unsigned long>(profiles[i].Data1),
                              profiles[i].Data2, profiles[i].Data3,
                              profiles[i].Data4[0], profiles[i].Data4[1],
                              profiles[i].Data4[2], profiles[i].Data4[3],
                              profiles[i].Data4[4], profiles[i].Data4[5],
                              profiles[i].Data4[6], profiles[i].Data4[7]);
                if (i) claimed += ' ';
                claimed += g;
            }
            REX_LOG_WARN(kModule,
                         "skipped (driver exposes {} profiles but none for "
                         "{}) - using software path. driver claims: {}",
                         m_hwDecoderProfiles,
                         avcodec_get_name(m_stream->codecpar->codec_id),
                         claimed.empty() ? "(none enumerated)" : claimed);
            tryHardware = false;
        }
    }
    if (tryHardware) {
        AVBufferRef* hwDevRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (hwDevRef) {
            AVHWDeviceContext* hwDev =
                reinterpret_cast<AVHWDeviceContext*>(hwDevRef->data);
            AVD3D11VADeviceContext* hwctx =
                reinterpret_cast<AVD3D11VADeviceContext*>(hwDev->hwctx);
            // Ownership transfer: FFmpeg never AddRefs a caller-provided
            // hwctx->device but its device-free Releases it once per
            // init/unref cycle (measured: -1 manager-device ref per HW
            // open/close; after two cycles the device died and reopen
            // failed GetDevice while PLAY presented stale slots — desktop
            // white noise). This AddRef is consumed by exactly that
            // Release, netting zero per cycle with no leak.
            hwctx->device = m_device11.Get();
            m_device11->AddRef();
            hwctx->BindFlags = D3D11_BIND_SHADER_RESOURCE;  // FFmpeg adds BIND_DECODER
            // Do NOT set MiscFlags = SHARED | SHARED_NTHANDLE here. FFmpeg
            // applies this field to every frame texture it allocates from the
            // device context, and decoder output textures carry
            // D3D11_BIND_DECODER - D3D11 refuses to share those (see the
            // class comment: CreateSharedHandle fails even with the flags).
            // Some drivers (Intel) also reject CreateTexture2D for the
            // combination, which makes FFmpeg fall back to software inside
            // get_format while the decoder still reports hardware. The frames
            // are copied into our own shareable pool, so the source textures
            // never need to be shared.
            const int initRet = av_hwdevice_ctx_init(hwDevRef);
            if (initRet >= 0) {
                m_decCtx->hw_device_ctx = av_buffer_ref(hwDevRef);
                m_decCtx->get_format = &FfmpegVideoDecoder::GetHwFormat;
                m_hwDevCtx = hwDevRef;
                // Frame-threading a hwaccel decoder is unsafe (one shared
                // hwctx), so pin hardware decode to a single thread. This is
                // exactly what LAV does for every hwaccel decoder
                // (d3d11va.cpp:668). Only the SOFTWARE path uses auto threads.
                m_decCtx->thread_count = 1;
                m_decCtx->thread_type = 0;
                const int hwOpenRet = avcodec_open2(m_decCtx, codec, nullptr);
                if (hwOpenRet < 0) {
                    char hwErr[AV_ERROR_MAX_STRING_SIZE] = {};
                    av_strerror(hwOpenRet, hwErr, sizeof(hwErr));
                    REX_LOG_WARN(kModule,
                                 "hardware decode open failed ({}: {}), retrying software",
                                 hwOpenRet, hwErr);
                    avcodec_free_context(&m_decCtx);
                    av_buffer_unref(&m_hwDevCtx);
                    m_hwDevCtx = nullptr;
                    // The failed hardware open consumed the codec context —
                    // rebuild a clean one for the software attempt. Without
                    // this the fallback below dereferences a null context
                    // (avcodec_free_context nulls the pointer).
                    m_decCtx = avcodec_alloc_context3(codec);
                    if (!m_decCtx ||
                        avcodec_parameters_to_context(
                            m_decCtx, m_stream->codecpar) < 0) {
                        Close();
                        return shared::Result<void>(
                            shared::ErrorCode::OutOfMemory,
                            "cannot recreate codec context for software "
                            "fallback");
                    }
                    m_decCtx->thread_count = 0;
                    m_decCtx->pkt_timebase = m_stream->time_base;
                } else {
                    m_hardware = true;
                }
            } else {
                char initErr[AV_ERROR_MAX_STRING_SIZE] = {};
                av_strerror(initRet, initErr, sizeof(initErr));
                REX_LOG_WARN(kModule,
                             "D3D11VA device init failed ({}: {}), falling back to software decode",
                             initRet, initErr);
                av_buffer_unref(&hwDevRef);
            }
        }
        if (!m_hardware) {
            REX_LOG_WARN(kModule, "D3D11VA unavailable, falling back to software decode");
        }
    }

    // â”€â”€ Software fallback â”€â”€
    if (!m_hardware) {
        if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
            Close();
            return shared::Result<void>(
                shared::ErrorCode::UnsupportedFormat,
                "software decoder open failed for " +
                    std::string(codec->name) + " (" + std::string(path) +
                    ") - transcode to H.264/HEVC or update the GPU driver");
        }
        if (!EnsureSws(m_width, m_height,
                       static_cast<AVPixelFormat>(m_stream->codecpar->format),
                       /*sixteenBit=*/false)) {
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
    m_reachedEnd.store(false, std::memory_order_release);
    m_pktPending = false;
    m_open.store(true, std::memory_order_release);

    // P13 diagnostics: exactly which codec/profile/pixel path/adapter is
    // in play. Answers "why is decode slow?" without a debugger.
    {
        const char* hwaccel = m_hardware ? "d3d11va" : "software";
        const char* pixfmt = av_get_pix_fmt_name(
            m_hardware ? m_decCtx->sw_pix_fmt
                       : static_cast<AVPixelFormat>(m_decCtx->pix_fmt));
        uint64_t luid = 0;
        if (m_device11) {
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(m_device11.As(&dxgiDev)) &&
                SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetDesc(&desc))) {
                luid = static_cast<uint64_t>(desc.AdapterLuid.LowPart) |
                       (static_cast<uint64_t>(desc.AdapterLuid.HighPart) << 32);
            }
        }
        REX_LOG_INFO(kModule,
                     "decoder: codec={} profile={} hwaccel={} pixelFormat={} "
                     "adapterLUID=0x{:016x}",
                     avcodec_get_name(m_decCtx->codec_id),
                     m_decCtx->profile, hwaccel,
                     pixfmt ? pixfmt : "unknown", luid);
        REX_LOG_INFO(kModule, "opened '{}' {}x{} @{:.2f} fps ({} decode)", path,
                     m_width, m_height, m_frameRate,
                     m_hardware ? "hardware" : "software");
    }
    return shared::Result<void>();
}

void FfmpegVideoDecoder::Close() {
    // P1-9 first: abort any thread blocked in av_read_frame BEFORE tearing
    // anything down (the controller joins the decode thread after this).
    m_stopRequested.store(true, std::memory_order_release);
    m_open.store(false, std::memory_order_release);

    // Codec-context exclusion: avcodec_free_context() parks FFmpeg's frame
    // workers by releasing their async lock - the same lock
    // avcodec_receive_frame() holds while it runs. Overlapping the two
    // double-releases it and trips av_assert0(fctx->async_lock)
    // (libavcodec/pthread_frame.c) - an abort that kills the runtime.
    // A concurrent Seek()'s avcodec_flush_buffers() does the same, and on a
    // freed context it is a use-after-free. Serialise the whole teardown.
    std::lock_guard<std::recursive_mutex> codecLock(m_codecMutex);

    if (m_decCtx) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
    }
    // NOTE: no Release for the hwctx->device ownership transfer here —
    // av_buffer_unref above ran FFmpeg's device-free, which consumed the
    // AddRef taken in Open. Releasing again would double-free.
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
    for (SwsContext* lane : m_swsLanes) {
        sws_freeContext(lane);
    }
    m_swsLanes.clear();
    m_swsLaneKey = SwsCacheKey{};

    m_videoStreamIndex = -1;
    m_stream = nullptr;
    m_pktPending = false;
    m_hardware = false;
    m_eof = false;
    m_reachedEnd.store(false, std::memory_order_release);
    m_unusableCode.store(0, std::memory_order_relaxed);  // P2-1: fresh Open = fresh verdict
    m_duration = 0.0;
    m_startTime = 0.0;
    m_frameRate = 30.0;
    m_width = 0;
    m_height = 0;

    // Keep the pool generation (textures + NT handles + video processor +
    // staging texture) alive: the renderer's D3D12 imports reference these
    // exact handles, and a rebuild on the next session would charge the
    // driver another permanent per-import footprint. Only per-session lease
    // bookkeeping is reset here; ReleasePool() is the teardown boundary.
    for (UINT i = 0; i < kPoolSize; ++i) m_slotFrameIndex[i] = 0;
    m_consumedFrameIndex.store(-1, std::memory_order_release);
    m_hdrContent = false;
    m_fenceValue = 0;
    m_frameIndex = 0;
    m_colorInfo = VideoColorInfo{};
    m_swsKey = SwsCacheKey{};

    m_fence11.Reset();
    m_context4.Reset();
    m_videoContext1.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_context.Reset();
    m_device11.Reset();
}

void FfmpegVideoDecoder::ReleasePool() {
    for (HANDLE h : m_poolHandles) {
        if (h) CloseHandle(h);
    }
    m_poolHandles.clear();
    m_poolTex.clear();
    m_staging.Reset();
    m_enum.Reset();
    m_processor.Reset();
    m_poolDevice.Reset();
    m_poolW = 0;
    m_poolH = 0;
    m_poolFormat = DXGI_FORMAT_UNKNOWN;
    m_poolGeneration = 0;
    for (UINT i = 0; i < kPoolSize; ++i) m_slotFrameIndex[i] = 0;

    // Retired generations hold shareable textures exactly like the active one;
    // they must be destroyed here or their NT handles (and VRAM) outlive the
    // decoder.
    for (auto& gen : m_poolCache) {
        for (HANDLE h : gen.handles) {
            if (h) CloseHandle(h);
        }
    }
    m_poolCache.clear();
}

// ─── Pool generation cache ───────────────────────────────────────────────
// See the CachedPool comment: rebuilding the pool hands the D3D12 renderer a
// fresh NT-handle generation, and the AMD driver never returns the VRAM it
// charges for those imports. Reuse by (geometry, format) turns the per-switch
// growth into a plateau.

size_t FfmpegVideoDecoder::FindPoolGeneration(const PoolGenerationKey* keys,
                                              size_t count,
                                              const PoolGenerationKey& key) {
    for (size_t i = 0; i < count; ++i) {
        if (keys[i].width == key.width && keys[i].height == key.height &&
            keys[i].format == key.format) {
            return i;
        }
    }
    return count;
}

size_t FfmpegVideoDecoder::PickLruPoolGeneration(const uint64_t* useTicks,
                                                 size_t count) {
    if (count == 0) return 0;
    size_t victim = 0;
    for (size_t i = 1; i < count; ++i) {
        if (useTicks[i] < useTicks[victim]) victim = i;
    }
    return victim;
}

void FfmpegVideoDecoder::RetireActivePool() {
    if (m_poolTex.empty() && m_poolHandles.empty()) return;

    CachedPool gen;
    gen.tex = std::move(m_poolTex);
    gen.handles = std::move(m_poolHandles);
    gen.vpEnum = std::move(m_enum);
    gen.vpProcessor = std::move(m_processor);
    gen.staging = std::move(m_staging);
    gen.device = std::move(m_poolDevice);
    gen.width = m_poolW;
    gen.height = m_poolH;
    gen.format = m_poolFormat;
    gen.generation = m_poolGeneration;
    gen.useTick = ++m_poolUseCounter;
    m_poolCache.push_back(std::move(gen));

    m_poolTex.clear();
    m_poolHandles.clear();
    m_enum.Reset();
    m_processor.Reset();
    m_staging.Reset();
    m_poolDevice.Reset();
    m_poolW = 0;
    m_poolH = 0;
    m_poolFormat = DXGI_FORMAT_UNKNOWN;
    m_poolGeneration = 0;
}

void FfmpegVideoDecoder::TrimPoolCache(size_t keep) {
    while (m_poolCache.size() > keep) {
        std::vector<uint64_t> ticks;
        ticks.reserve(m_poolCache.size());
        for (const auto& gen : m_poolCache) ticks.push_back(gen.useTick);
        const size_t victim = PickLruPoolGeneration(ticks.data(), ticks.size());
        for (HANDLE h : m_poolCache[victim].handles) {
            if (h) CloseHandle(h);
        }
        REX_LOG_INFO(kModule,
                     "pool generation retired: {}x{} fmt={} gen={} "
                     "(cache over {} live generations)",
                     m_poolCache[victim].width, m_poolCache[victim].height,
                     static_cast<int>(m_poolCache[victim].format),
                     m_poolCache[victim].generation, keep + 1);
        m_poolCache.erase(m_poolCache.begin() + victim);
    }
}

bool FfmpegVideoDecoder::ReuseCachedPool(UINT width, UINT height,
                                         DXGI_FORMAT format) {
    if (m_poolCache.empty()) return false;

    std::vector<PoolGenerationKey> keys;
    keys.reserve(m_poolCache.size());
    for (const auto& gen : m_poolCache) {
        keys.push_back({gen.width, gen.height, gen.format});
    }
    const size_t hit = FindPoolGeneration(
        keys.data(), keys.size(), PoolGenerationKey{width, height, format});
    if (hit == keys.size()) return false;
    // A re-created D3D11 device invalidates every texture from the old one.
    if (m_poolCache[hit].device.Get() != m_device11.Get()) return false;

    // Park the active generation first (index-stable: push_back appends).
    RetireActivePool();

    CachedPool& gen = m_poolCache[hit];
    m_poolTex = std::move(gen.tex);
    m_poolHandles = std::move(gen.handles);
    m_enum = std::move(gen.vpEnum);
    m_processor = std::move(gen.vpProcessor);
    m_staging = std::move(gen.staging);
    m_poolDevice = std::move(gen.device);
    m_poolW = gen.width;
    m_poolH = gen.height;
    m_poolFormat = gen.format;
    m_poolGeneration = gen.generation;
    for (UINT i = 0; i < kPoolSize; ++i) m_slotFrameIndex[i] = 0;

    REX_LOG_INFO(kModule,
                 "pool generation reused: {}x{} fmt={} gen={} "
                 "(cached, renderer imports stay valid)",
                 m_poolW, m_poolH, static_cast<int>(m_poolFormat),
                 m_poolGeneration);
    m_poolCache.erase(m_poolCache.begin() + hit);
    return true;
}

bool FfmpegVideoDecoder::Seek(double seconds) {
    if (!m_open.load(std::memory_order_acquire)) return false;
    // Codec-context exclusion: the flush below must not overlap a receive
    // on a frame-threaded decoder (FFmpeg's async lock would be released
    // twice -> av_assert0 in libavcodec/pthread_frame.c -> abort).
    std::lock_guard<std::recursive_mutex> codecLock(m_codecMutex);
    const bool ok = SeekInternal(seconds);
    // A user seek starts a new stretch of content; the end observed before
    // the seek must not keep the controller's scene-end latch armed. The
    // internal loop rewind deliberately does NOT do this - the controller
    // reads the end between the rewind and the next end of content.
    if (ok) m_reachedEnd.store(false, std::memory_order_release);
    return ok;
}

bool FfmpegVideoDecoder::Flush() {
    std::lock_guard<std::recursive_mutex> codecLock(m_codecMutex);
    if (!m_decCtx) return false;
    avcodec_flush_buffers(m_decCtx);
    m_eof = false;
    return true;
}

enum AVPixelFormat FfmpegVideoDecoder::GetHwFormat(
    AVCodecContext* ctx, const enum AVPixelFormat* const formats) {
    // The D3D11VA decoder delivers its frames as AV_PIX_FMT_D3D11. The
    // caller (FFmpeg) has already checked that the codec can produce them.
    for (const enum AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f != AV_PIX_FMT_D3D11) {
            continue;
        }
        // Do NOT pre-allocate the hardware frames context here.
        //
        // FFmpeg's D3D11VA decoder creates the decode texture ITSELF (it
        // keeps it in the frames context's user_opaque) and its pool_init()
        // refuses a context the caller allocated: "AVD3D11VAFramesContext.
        // texture not set" -> "Failed setup for format d3d11" -> the hwaccel
        // never initialises and the stream falls back to SOFTWARE decode.
        // Measured 2026-10-21 on a 4K 60 fps 8-bit HEVC clip (the user's
        // own wallpaper): pre-sized -> software decodePath=SW_BGRA8 and a
        // ~170 ms late frame every 2.7 s; without the pre-sizing the same
        // clip decodes on the GPU. The allocation churn this was meant to
        // avoid (a re-allocation when the resolution changes mid-stream) is
        // a far smaller cost than a 4K software decode.
        return AV_PIX_FMT_D3D11;
    }
    // FFmpeg did not offer D3D11 for this stream: it will decode in software
    // inside the codec (get_format is the pivot). Name the format it chose and
    // the codec/profile so a hardware->software demotion is diagnosable; the
    // old code returned silently and the runtime still logged "hardware
    // decode" from the successful avcodec_open2 above.
    const char* first = av_get_pix_fmt_name(formats[0]);
    REX_LOG_WARN(kModule,
                 "D3D11VA not offered for codec={} profile={} (first format {}), decoding in software",
                 avcodec_get_name(ctx->codec_id), ctx->profile,
                 first ? first : "none");
    return formats[0];
}

bool FfmpegVideoDecoder::SeekInternal(double seconds) {
    std::lock_guard<std::recursive_mutex> codecLock(m_codecMutex);
    if (!m_fmtCtx || !m_stream) return false;
    // Close() may have freed the context between the caller's open check and
    // here; a flush on a freed/null context is a crash (FFmpeg asserts).
    if (!m_decCtx) return false;
    const auto tSeek0 = Clock::now();
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
        // Loop wraps log one seek line per wrap (10-30 s cadence on typical
        // loop files) - the only in-the-wild measure of loop-restart cost.
        // Measured warm-cache: ~0.1 ms; cold reads can be 10x higher.
        REX_LOG_INFO(kModule, "seek to {:.2f}s took {:.1f} ms", seconds,
                     std::chrono::duration<double, std::milli>(Clock::now() - tSeek0)
                         .count());
        return true;
    }
    REX_LOG_WARN(kModule, "seek to {:.2f}s failed (av_seek_frame {})", seconds, ret);
    return false;
}

void FfmpegVideoDecoder::SelectStreamByResolution(int targetWidth, int targetHeight) {
    if (!m_fmtCtx || !m_open.load(std::memory_order_acquire)) return;

    // Codec-context exclusion: the stream switch frees and reopens the codec
    // context, so it must not overlap a decode (the frame-thread async lock
    // would be released twice -> FFmpeg abort).
    std::lock_guard<std::recursive_mutex> codecLock(m_codecMutex);

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
    // Pass 2: nothing fits â€” take the smallest stream available.
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

    // â”€â”€ Re-open the codec on the new stream â”€â”€
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
        m_decCtx->get_format = &FfmpegVideoDecoder::GetHwFormat;
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
        if (!EnsureSws(newStream->codecpar->width, newStream->codecpar->height,
                       static_cast<AVPixelFormat>(newStream->codecpar->format),
                       /*sixteenBit=*/false)) {
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
    m_startTime = m_fmtCtx->start_time != AV_NOPTS_VALUE
                      ? static_cast<double>(m_fmtCtx->start_time) / AV_TIME_BASE
                      : 0.0;
    if (m_stream->r_frame_rate.den > 0) {
        m_frameRate = static_cast<double>(m_stream->r_frame_rate.num) /
                      m_stream->r_frame_rate.den;
    }
    {
        // P0-1: same measured-rate preference as Open (see ResolveFrameRate).
        bool usedCount = false;
        const double declared = m_frameRate;
        m_frameRate = ResolveFrameRate(m_stream->nb_frames, m_duration,
                                       declared, &usedCount);
        if (usedCount &&
            (declared <= 0.0 || declared / m_frameRate < 0.95 ||
             declared / m_frameRate > 1.05)) {
            REX_LOG_WARN(kModule,
                         "frame rate {:.2f} fps from {} frames / {:.2f} s, "
                         "not the declared {:.2f} fps",
                         m_frameRate, m_stream->nb_frames, m_duration,
                         declared);
        }
    }
    if (m_frameRate <= 0.0) {
        m_frameRate = 30.0;
    }
    m_width  = newStream->codecpar->width;
    m_height = newStream->codecpar->height;
    m_eof = false;
    m_reachedEnd.store(false, std::memory_order_release);
    m_pktPending = false;
    REX_LOG_INFO(kModule, "stream switched: {}x{} @{:.2f} fps ({} decode)", m_width,
                 m_height, m_frameRate, m_hardware ? "hardware" : "software");
}

// â”€â”€â”€ Colorimetry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

VideoColorInfo FfmpegVideoDecoder::ResolveColorInfo() const {
    VideoColorInfo info;
    info.srcWidth  = m_frame ? m_frame->width  : m_width;
    info.srcHeight = m_frame ? m_frame->height : m_height;

    // Fallback chain: frame properties â†’ codec context (derived from the
    // stream at avcodec_open2) â†’ reasonable defaults.
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
    // Codec-context exclusion: the receive below must not overlap a flush
    // (Seek/Stop) or a close. On a frame-threaded decoder FFmpeg releases its
    // async lock in both, and a double release trips av_assert0 in
    // libavcodec/pthread_frame.c - an abort that kills the runtime.
    std::lock_guard<std::recursive_mutex> codecLock(m_codecMutex);

    // Zero-progress guard. Measured 2026-11: this FFmpeg build's SOFTWARE AV1
    // decoder opens successfully ("software decode", pix_fmt yuv420p) but emits
    // ZERO frames, repeating "Failed to get pixel format" forever. The hardware
    // path decodes the same file fine, so the failure is invisible in normal
    // use — but if hardware decode ever fails on a user's machine, the
    // wallpaper would sit there rendering nothing with no error anywhere.
    // The roadmap forbids that: an unsupported path must produce an explicit,
    // actionable error, never a silent stall.
    if (m_framesDecoded.load(std::memory_order_relaxed) == 0) {
        if (++m_zeroProgressCalls == kZeroProgressReportAfter) {
            REX_LOG_ERROR(kModule,
                         "no frame decoded after {} attempts (codec={} "
                         "hwaccel={}, pix_fmt={}). This build cannot decode "
                         "this stream in {} — the wallpaper will not play.",
                         m_zeroProgressCalls, avcodec_get_name(m_decCtx->codec_id),
                         m_hardware.load(std::memory_order_relaxed) ? "d3d11va"
                                                                    : "software",
                         static_cast<int>(m_decCtx->pix_fmt),
                         m_hardware.load(std::memory_order_relaxed) ? "hardware"
                                                                    : "software");
            // P2-1: latch the unusable path so telemetry (and through it the
            // supervisor's poison-file park) sees it instead of stalling
            // silently forever. Write-once per Open; Close() clears it.
            if (DecodePathUnusable(0, m_zeroProgressCalls,
                                   kZeroProgressReportAfter)) {
                m_unusableCode.store(1, std::memory_order_relaxed);
                REX_LOG_ERROR(kModule,
                             "decode path latched unusable (code=1, zero "
                             "progress) - supervisor should park this file");
            }
        }
    } else {
        m_zeroProgressCalls = 0;
    }

    const auto t0 = Clock::now();
    EofLoopGuard eofGuard;
    ++m_dbgCalls;
    if ((m_dbgCalls % 600) == 0 && std::getenv("REX_SCHED_DEBUG")) {
        REX_LOG_INFO(kModule,
                     "decodeframe calls={} videoPkgs={} receives={} "
                     "(pkgs/call={:.2f} recv/call={:.2f})",
                     m_dbgCalls, m_dbgPackets, m_dbgReceives,
                     static_cast<double>(m_dbgPackets) / m_dbgCalls,
                     static_cast<double>(m_dbgReceives) / m_dbgCalls);
    }
    // Per-frame stage accumulators (diagnostic): demux + send + receive time
    // across the whole call, attributed to the frame it eventually produces.
    long long stageDemuxUs = 0, stageSendUs = 0, stageRecvUs = 0;
    while (true) {
        const auto tRecv0 = Clock::now();
        const int r = avcodec_receive_frame(m_decCtx, m_frame);
        stageRecvUs +=
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                  tRecv0)
                .count();
        if (r == 0) {
            ++m_dbgReceives;
            // Resolve colorimetry of this frame (frame side data may change
            // mid-stream). Drives pool format, VP color spaces and the
            // renderer's shader constants.
            m_colorInfo = ResolveColorInfo();
            m_hdrContent = m_colorInfo.transfer != VideoColorInfo::Transfer::SDR;
            outFrame.colorInfo = m_colorInfo;
            const auto tConv0 = Clock::now();
            const bool ok = ConvertAndSignal(outFrame);
            const auto tConv1 = Clock::now();
            const auto tNow = Clock::now();
            // Per-frame stage sample (diagnostic ring, REX_PERFRAME_CSV=1
            // dumps it). Arrival = wall gap since the previous produced
            // frame; queueDepth is stamped by the controller (-1 = unknown).
            DecodeStage st{};
            st.seq = m_stageSeq++;
            st.frameIndex = static_cast<int64_t>(m_frameIndex);
            st.demuxUs = stageDemuxUs;
            st.sendUs = stageSendUs;
            st.recvUs = stageRecvUs;
            st.convUs =
                std::chrono::duration_cast<std::chrono::microseconds>(tConv1 -
                                                                      tConv0)
                    .count();
            st.arrivalUs =
                m_haveLastProduce
                    ? std::chrono::duration_cast<std::chrono::microseconds>(
                          tNow - m_lastProduce)
                          .count()
                    : 0;
            st.queueDepth = -1;
            m_lastProduce = tNow;
            m_haveLastProduce = true;
            m_stageRing.Push(st);
            const double decodeMs = ms(tNow - t0).count();
            m_lastDecodeMs.store(decodeMs, std::memory_order_relaxed);
            m_decodeMsSum.fetch_add(decodeMs, std::memory_order_relaxed);
            // P13 ring sample (decode = receive_frame â†’ pool publish).
            m_decodeMsRing[m_decodeMsCount % kTimingRingSize] =
                static_cast<float>(decodeMs);
            ++m_decodeMsCount;
            if (ok) {
                m_framesDecoded.fetch_add(1, std::memory_order_relaxed);
            }
            return ok;
        }
        if (r == AVERROR(EAGAIN)) {
            // decoder needs more packets; fall through to read one
        } else if (r == AVERROR_EOF) {
            if (m_eof) {
                // The delayed-frame flush above is complete: the VIDEO stream
                // has really ended. Latch it for the controller - the
                // container can continue past this point (an audio tail), so
                // a PTS-vs-container-duration comparison never fires for
                // those files (measured on a 191.49 s trailer whose last
                // video frame is at 189.87 s).
                m_reachedEnd.store(true, std::memory_order_release);
                // P1-10: one internal seek per call. A second consecutive
                // EOF means the seek produced no frames (0-frame file):
                // return instead of cycling forever.
                if (m_looping.load(std::memory_order_relaxed) &&
                    eofGuard.AllowSeek() && SeekInternal(0.0)) {
                    continue;
                }
                return false;
            }
            m_eof = true;
            avcodec_send_packet(m_decCtx, nullptr);  // flush remaining frames
            continue;
        } else {
            REX_LOG_ERROR(kModule, "avcodec_receive_frame failed ({})", r);
            return false;        }

        if (!m_pktPending) {
            const auto tDemux0 = Clock::now();
            const int pr = av_read_frame(m_fmtCtx, m_pkt);
            stageDemuxUs +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - tDemux0)
                    .count();
            if (pr == AVERROR_EXIT) {
                // Interrupted by RequestStop()/Close() — prompt shutdown,
                // not an error (stays silent to avoid log spam per close).
                return false;
            }
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
        const auto tSend0 = Clock::now();
        int s = avcodec_send_packet(m_decCtx, m_pkt);
        stageSendUs +=
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                  tSend0)
                .count();
        if (s == AVERROR(EAGAIN)) {
            // The decoder did NOT accept the packet, so it is still ours and
            // must NOT be unreffed — doing so punches a permanent hole in the
            // compressed stream (a dropped frame plus a decoder fed a
            // discontinuity), which is intermittent-stutter shaped rather than
            // a constant rate deficit.
            //
            // The retry is bounded because D3D11VA can return EAGAIN from send
            // while receive also returns EAGAIN (saturated surface pool), and
            // an unbounded retry spins forever holding the decode mutex.
            // ShouldRetrySendPacket consumes one retry and RESETS the budget
            // when spent, so the next packet starts fresh instead of being
            // dropped instantly for the rest of the stream (audit D1).
            if (rex::wallpaper::ShouldRetrySendPacket(m_sendEagainRetries,
                                                      kMaxSendEagainRetries)) {
                continue;
            }
            REX_LOG_WARN(kModule, "avcodec_send_packet stuck at EAGAIN ({} "
                         "retries) — advancing demuxer; frame may be lost",
                         kMaxSendEagainRetries);
            m_sendEagainStalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_sendEagainRetries = 0;
        }
        av_packet_unref(m_pkt);
        m_pktPending = false;
        if (s == 0) {
            m_sendFailStreak = 0;
            ++m_dbgPackets;  // video packet accepted (decode-thread owned)
            // Only clear the "unusable path" latch once a frame has actually
            // been decoded. A decoder that accepts packets but emits nothing
            // would otherwise re-announce on every accepted packet.
            if (m_framesDecoded.load(std::memory_order_relaxed) > 0) {
                m_sendFailReported = false;
            }
            continue;
        }
        // A decoder that opened but can never accept a packet (e.g. software
        // AV1 on a build with no working AV1 software decoder: opens, then
        // returns EINVAL forever) used to spam one identical ERROR per
        // packet — measured 771 lines in ~12 s, drowning the real diagnostic.
        // Escalate once per streak, then stay quiet until a send succeeds.
        ++m_sendFailStreak;
        // Escalate once per session when the decode path has never produced a
        // frame. Gating on a consecutive-failure streak is not enough: a
        // decoder that opens but cannot decode AV1 in software still ACCEPTS
        // some packets (s==0) while emitting nothing, so the streak keeps
        // resetting and the error re-announces — measured 769 lines in 12 s.
        // "Zero frames ever decoded" is the real unusable-path predicate, and
        // matches the zero-progress guard above.
        const bool neverDecoded =
            m_framesDecoded.load(std::memory_order_relaxed) == 0;
        if (neverDecoded && m_sendFailStreak >= kSendFailReportAfter &&
            !m_sendFailReported) {
            m_sendFailReported = true;
            REX_LOG_ERROR(kModule,
                         "avcodec_send_packet failed ({}) {} times in a row and "
                         "no frame has ever decoded (codec={}, hwaccel={}, "
                         "pix_fmt={}). This build cannot decode this stream in "
                         "{} — the wallpaper will not play.",
                         s, m_sendFailStreak,
                         avcodec_get_name(m_decCtx->codec_id),
                         m_hardware.load(std::memory_order_relaxed) ? "d3d11va"
                                                                    : "software",
                         static_cast<int>(m_decCtx->pix_fmt),
                         m_hardware.load(std::memory_order_relaxed) ? "hardware"
                                                                    : "software");
            // P2-1: latch the unusable path (code=2, send-fail streak on a
            // never-decoded path). Telemetry carries it to the supervisor.
            m_unusableCode.store(2, std::memory_order_relaxed);
        } else if (m_sendFailStreak > kSendFailReportAfter) {
            // Already reported for this streak; stay silent (debug builds show
            // the count) rather than repeating the same line per packet.
            REX_LOG_DEBUG(kModule, "avcodec_send_packet still failing ({})",
                          s);
        } else if (!neverDecoded) {
            // A healthy decode path can still hit a transient packet error;
            // that is worth one line each.
            REX_LOG_ERROR(kModule, "avcodec_send_packet failed ({})", s);
        }
        return false;
    }
}

UINT FfmpegVideoDecoder::PickPoolSlot() const {
    const int64_t consumed = m_consumedFrameIndex.load(std::memory_order_acquire);
    const bool haveConsumed = consumed >= 0;
    return PickSlotOldestSafe(m_slotFrameIndex, kPoolSize, consumed,
                              kSlotReuseMargin, haveConsumed);
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

// P13: percentile over a fixed ring of the most recent samples.
static double PercentileOfRing(const float* ring, size_t count, double p) {
    if (count == 0) return 0.0;
    const size_t n = std::min(count, FfmpegVideoDecoder::kTimingRingSize);
    std::vector<float> sorted(ring, ring + static_cast<long>(n));
    const size_t idx = static_cast<size_t>(
        std::clamp(p, 0.0, 1.0) * static_cast<double>(n - 1));
    std::nth_element(sorted.begin(),
                     sorted.begin() + static_cast<long>(idx),
                     sorted.end());
    return static_cast<double>(sorted[idx]);
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
    d.sendEagainStalls = m_sendEagainStalls.load(std::memory_order_relaxed);
    d.pathUnusable = m_unusableCode.load(std::memory_order_relaxed) != 0;
    d.hwDecoderProfiles = m_hwDecoderProfiles;
    d.decodeMsP50 = PercentileOfRing(m_decodeMsRing.data(), m_decodeMsCount, 0.50);
    d.decodeMsP95 = PercentileOfRing(m_decodeMsRing.data(), m_decodeMsCount, 0.95);
    d.decodeMsP99 = PercentileOfRing(m_decodeMsRing.data(), m_decodeMsCount, 0.99);
    d.vpMsP50     = PercentileOfRing(m_vpMsRing.data(),     m_vpMsCount,     0.50);
    d.vpMsP95     = PercentileOfRing(m_vpMsRing.data(),     m_vpMsCount,     0.95);
    d.vpMsP99     = PercentileOfRing(m_vpMsRing.data(),     m_vpMsCount,     0.99);
    return d;
}

void FfmpegVideoDecoder::StampStageQueueDepth(int64_t depth) {
    DecodeStage* back = m_stageRing.Back();
    if (back) back->queueDepth = depth;
}

std::vector<DecodeStage> FfmpegVideoDecoder::SnapshotStages() const {
    return m_stageRing.Snapshot();
}

size_t FfmpegVideoDecoder::DumpDecodeStages() const {
    if (!std::getenv("REX_PERFRAME_CSV")) return 0;
    const std::vector<DecodeStage> rows = m_stageRing.Snapshot();
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return 0;
    std::wstring path(tmp);
    path += L"Paiint\\perframe_decode.csv";
    FILE* f = nullptr;
    // Plain narrow mode: "ccs=UTF-8" would switch the stream wide-oriented
    // and every narrow fputs below would fail silently (measured: BOM-only
    // file). Content is pure ASCII; no BOM needed.
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return 0;
    fputs(DecodeStageCsvHeader(), f);
    fputc('\n', f);
    for (const auto& s : rows) {
        const std::string row = DecodeStageCsvRow(s);
        fputs(row.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
    return rows.size();
}

UINT FfmpegVideoDecoder::PickSlotOldestSafe(const int64_t* slotFrameIndex,
                                           UINT slots, int64_t consumed,
                                           int64_t margin, bool haveConsumed) {
    // Fallback: the strict oldest slot, always a valid choice.
    UINT fallback = 0;
    int64_t fallbackIndex = slotFrameIndex[0];
    for (UINT i = 1; i < slots; ++i) {
        const int64_t idx = slotFrameIndex[i];
        if (idx < fallbackIndex) { fallbackIndex = idx; fallback = i; }
    }
    if (!haveConsumed) return fallback;

    // Preferred: the oldest slot at least `margin` frames behind the render
    // thread. Found by scan rather than seeded at slot 0, because slot 0 may
    // itself be ineligible - seeding would return it unchecked.
    UINT best = UINT_MAX;
    int64_t bestIndex = INT64_MAX;
    for (UINT i = 0; i < slots; ++i) {
        const int64_t idx = slotFrameIndex[i];
        if (idx <= consumed - margin && idx < bestIndex) {
            bestIndex = idx;
            best = i;
        }
    }
    return best != UINT_MAX ? best : fallback;
}

// P0-3 / R3+R4: the pool is keyed by (size, exact format), not by
// (size, hdr flag + m_processor). The old fast path required m_processor,
// null on software decode, so SW rebuilt all 8 textures + shared handles
// EVERY frame (exhausting the renderer's slot cache); and the format came
// from m_hdrContent alone, so RGB bytes landed in a YUV pool.
DXGI_FORMAT FfmpegVideoDecoder::PoolTextureFormat(bool hardwarePath,
                                                  bool hdrContent,
                                                  DXGI_FORMAT surfaceFormat) {
    if (hardwarePath) {
        // The pool must mirror the decoder surface exactly: a format
        // mismatch in CopySubresourceRegion is dropped silently and the
        // pool stays uninitialised (the 10-bit SDR "green mess" — Main10/
        // bt709 decoding to P010 surfaces while the SDR flag chose NV12).
        if (surfaceFormat == DXGI_FORMAT_NV12 ||
            surfaceFormat == DXGI_FORMAT_P010) {
            return surfaceFormat;
        }
        // Unhandled surface format: historical flag-based fallback (the HW
        // copy path logs the surface format and MISMATCH for diagnosis).
        return hdrContent ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }
    // Software path: swscale writes packed RGB (BGRA SDR / RGBA64 HDR).
    return hdrContent ? DXGI_FORMAT_R16G16B16A16_UNORM
                      : DXGI_FORMAT_B8G8R8A8_UNORM;
}

bool FfmpegVideoDecoder::EnsurePool(UINT width, UINT height,
                                    DXGI_FORMAT format) {
    if (m_poolW == width && m_poolH == height &&
        m_poolTex.size() == kPoolSize && m_poolFormat == format &&
        m_poolDevice.Get() == m_device11.Get()) {
        return true;
    }

    // Drop retired generations built on a previous D3D11 device: they cannot
    // serve this session and would pin their VRAM until process exit.
    for (auto it = m_poolCache.begin(); it != m_poolCache.end();) {
        if (it->device.Get() != m_device11.Get()) {
            for (HANDLE h : it->handles) {
                if (h) CloseHandle(h);
            }
            it = m_poolCache.erase(it);
        } else {
            ++it;
        }
    }

    // Reuse a generation this decoder already built for the same key. The
    // renderer's imports for it are still valid, so no new handle generation
    // reaches ID3D12Device::OpenSharedHandle (see CachedPool).
    if (ReuseCachedPool(width, height, format)) {
        return true;
    }

    // Audit 2026-10-17, §5.2 #2: build the whole pool generation into
    // temporaries and commit only when every texture, NT handle, video
    // processor and staging texture exists. The old order published
    // m_poolW/H/Format (and resized the vectors) before the creation loop
    // and returned false mid-loop on failure — the fast path above then
    // claimed a complete pool whose remaining slots were null textures,
    // and every later conversion wrote through one.
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> tex(kPoolSize);
    std::vector<HANDLE> handles(kPoolSize, nullptr);
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vpProcessor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;

    const char* fmtName = "unknown";
    switch (format) {
        case DXGI_FORMAT_NV12:               fmtName = "NV12 (SDR bypass VP)"; break;
        case DXGI_FORMAT_P010:               fmtName = "P010 (GPU HDR path)"; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:     fmtName = "BGRA (SW SDR)"; break;
        case DXGI_FORMAT_R16G16B16A16_UNORM: fmtName = "RGBA16 (SW HDR)"; break;
        default: break;
    }
    // Every failure path below releases the partial generation and leaves
    // the previous committed pool (if any) intact; the next call retries.

    // SDR: NV12 pool (bypass VP, shader does YUV→RGB).
    // HDR + HW: P010 pool (GPU-resident, shader does BT.2020 PQ decode).
    // HDR + SW: RGBA16_UNORM pool (software swscale outputs RGBA64).
    // SDR + SW: BGRA pool (software swscale outputs BGRA).
    const DXGI_FORMAT poolFormat = format;

    for (size_t i = 0; i < tex.size(); ++i) {
        D3D11_TEXTURE2D_DESC pd = {};
        pd.Width = width;
        pd.Height = height;
        pd.MipLevels = 1;
        pd.ArraySize = 1;
        pd.Format = poolFormat;
        pd.SampleDesc.Count = 1;
        pd.Usage = D3D11_USAGE_DEFAULT;
        // P010 (multi-planar) doesn't support RENDER_TARGET; NV12 and RGBA16 do.
        pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (poolFormat == DXGI_FORMAT_NV12 ||
            poolFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            poolFormat == DXGI_FORMAT_R16G16B16A16_UNORM) {
            pd.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        pd.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(m_device11->CreateTexture2D(&pd, nullptr, &tex[i]))) {
            REX_LOG_ERROR(kModule, "pool texture creation failed ({}x{}, {})",
                          width, height, fmtName);
            for (HANDLE h : handles) { if (h) CloseHandle(h); }
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> res1;
        if (FAILED(tex[i].As(&res1)) ||
            FAILED(res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                            &handles[i])) ||
            !handles[i]) {
            // A slot without an NT handle can never reach the renderer; a
            // half-shareable pool is the exact state §5.2 #2 warns about.
            REX_LOG_ERROR(kModule, "pool slot share-handle creation failed");
            for (HANDLE h : handles) { if (h) CloseHandle(h); }
            return false;
        }
    }

    // Hardware YUV->RGB video processor (only needed for hardware decode)
    if (m_hardware) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = { 30, 1 };
        cd.InputWidth = width;
        cd.InputHeight = height;
        cd.OutputFrameRate = { 30, 1 };
        cd.OutputWidth = width;
        cd.OutputHeight = height;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum);
        if (SUCCEEDED(hr)) {
            hr = m_videoDevice->CreateVideoProcessor(vpEnum.Get(), 0, &vpProcessor);
        }
        if (FAILED(hr)) {
            REX_LOG_ERROR(kModule, "create video processor failed (0x{:08x})", hr);
            for (HANDLE h : handles) { if (h) CloseHandle(h); }
            return false;
        }
    }

    // Software-upload staging texture.
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = poolFormat;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device11->CreateTexture2D(&sd, nullptr, &staging))) {
        REX_LOG_ERROR(kModule, "create staging texture failed");
        for (HANDLE h : handles) { if (h) CloseHandle(h); }
        return false;
    }

    // ── Commit point (audit §5.2 #2): every resource exists; only now
    // invalidate the old generation and publish the new one.
    //
    // The outgoing generation is retired, not destroyed: its textures and NT
    // handles stay alive in the cache so switching back to this geometry/
    // format reuses them instead of charging the driver another permanent
    // import footprint (see CachedPool).
    RetireActivePool();

    m_poolTex = std::move(tex);
    m_poolHandles = std::move(handles);
    m_poolW = width;
    m_poolH = height;
    m_poolFormat = format;
    m_poolDevice = m_device11;
    m_poolGeneration = ++m_nextPoolGeneration;
    for (UINT i = 0; i < kPoolSize; ++i) m_slotFrameIndex[i] = 0;
    m_enum = std::move(vpEnum);
    m_processor = std::move(vpProcessor);
    m_staging = std::move(staging);

    // Active + retired must stay within kMaxLivePoolGenerations.
    TrimPoolCache(kMaxLivePoolGenerations - 1);

    REX_LOG_INFO(kModule,
                 "pool rebuilt: {} slots, {}x{}, {} gen={} (bounded)",
                 kPoolSize, width, height, fmtName, m_poolGeneration);
    return true;
}

bool FfmpegVideoDecoder::ConvertAndSignal(VideoFrame& outFrame) {
    // True software decode (no D3D11VA): always use software path.
    if (m_frame->format != AV_PIX_FMT_D3D11) {
        return ConvertSoftware(outFrame);
    }

    // HW-decoded frame: log surface format once per session.
    auto* tex11 = reinterpret_cast<ID3D11Texture2D*>(m_frame->data[0]);
    const UINT arraySlice = static_cast<UINT>(
        reinterpret_cast<intptr_t>(m_frame->data[1]));
    D3D11_TEXTURE2D_DESC desc11 = {};
    tex11->GetDesc(&desc11);

    const char* fmtName = "unknown";
    switch (desc11.Format) {
        case DXGI_FORMAT_NV12:       fmtName = "NV12"; break;
        case DXGI_FORMAT_P010:       fmtName = "P010"; break;
        case DXGI_FORMAT_YUY2:       fmtName = "YUY2"; break;
        case DXGI_FORMAT_AYUV:       fmtName = "AYUV"; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: fmtName = "BGRA"; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: fmtName = "RGBA16F"; break;
    }
    const DXGI_FORMAT poolFormat =
        PoolTextureFormat(true /* GPU path */, m_hdrContent, desc11.Format);
    // Log whenever the surface CHANGES, not once per process: the pool format
    // mirrors the surface, so a per-clip record is the first thing to read
    // when frames come out wrong (the old once-per-session static hid every
    // clip after the first — exactly how the 10-bit SDR bug stayed invisible).
    if (desc11.Format != m_loggedSurfaceFormat ||
        desc11.Width != m_loggedSurfaceW ||
        desc11.Height != m_loggedSurfaceH) {
        REX_LOG_INFO(kModule, "decoder surface: {}x{} fmt={} (0x{:x}) "
                     "hdr={} pool={} copy={}",
                     desc11.Width, desc11.Height, fmtName,
                     static_cast<unsigned>(desc11.Format), m_hdrContent,
                     poolFormat == DXGI_FORMAT_P010 ? "P010" :
                     (poolFormat == DXGI_FORMAT_NV12 ? "NV12" : "other"),
                     poolFormat == desc11.Format ? "match" : "MISMATCH");
        m_loggedSurfaceFormat = desc11.Format;
        m_loggedSurfaceW = desc11.Width;
        m_loggedSurfaceH = desc11.Height;
    }

    if (!EnsurePool(desc11.Width, desc11.Height, poolFormat)) {
        return false;
    }

    const UINT slot = PickPoolSlot();
    m_slotFrameIndex[slot] = static_cast<int64_t>(m_frameIndex);

    // GPU-resident path: copy decoder surface directly into pool.
    // SDR: NV12 → NV12 pool, shader does BT.709 YUV→RGB.
    // HDR: P010 → P010 pool, shader does BT.2020 Y′CbCr → PQ EOTF → RGB.
    const UINT srcSubresource = arraySlice;
    D3D11_BOX srcBox = { 0, 0, 0,
                         desc11.Width, desc11.Height, 1 };
    m_context->CopySubresourceRegion(m_poolTex[slot].Get(), 0, 0, 0, 0,
                                      tex11, srcSubresource, &srcBox);

    // Audit 2026-10-17 #5: draw from the process-wide sequence and enqueue
    // the signal as one step — the shared fence stays monotonic across
    // decoder sessions and instances, so a consumer can never be released
    // by a stale higher value before this frame's copy has executed.
    const UINT64 fenceValue = SignalSharedFenceOrdered(
        [&](UINT64 v) { m_context4->Signal(m_fence11.Get(), v); });
    m_fenceValue = fenceValue;
    m_slotFence[slot].store(fenceValue, std::memory_order_release);
    m_context->Flush();

    outFrame.sharedHandle = m_poolHandles[slot];
    outFrame.slot = slot;
    outFrame.fenceValue = fenceValue;
    outFrame.poolGeneration = m_poolGeneration;
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

bool FfmpegVideoDecoder::EnsureSws(int w, int h, AVPixelFormat srcFormat,
                                   bool sixteenBit) {
    const VideoColorInfo info = ResolveColorInfo();
    const SwsColorParams p = SwsColorParamsFor(info.yuvMatrix, info.fullRange);
    const SwsCacheKey want{srcFormat, sixteenBit, p.srcCsp, p.srcRange, w, h};
    if (m_sws != nullptr && SwsCacheHit(m_swsKey, want)) {
        return true;
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_sws = sws_getContext(w, h, srcFormat, w, h,
                           sixteenBit ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_BGRA,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        m_swsKey = SwsCacheKey{};
        return false;
    }
    sws_setColorspaceDetails(m_sws, sws_getCoefficients(p.srcCsp), p.srcRange,
                             sws_getCoefficients(p.dstCsp), p.dstRange, 0,
                             1 << 16, 1 << 16);
    m_swsKey = want;
    return true;
}

bool FfmpegVideoDecoder::EnsureSwsLanes(int lanes, int w, int h,
                                        AVPixelFormat srcFormat,
                                        bool sixteenBit) {
    if (lanes <= 1) return false;
    const VideoColorInfo info = ResolveColorInfo();
    const SwsColorParams p = SwsColorParamsFor(info.yuvMatrix, info.fullRange);
    const SwsCacheKey want{srcFormat, sixteenBit, p.srcCsp, p.srcRange, w, h};
    if (static_cast<int>(m_swsLanes.size()) == lanes &&
        SwsCacheHit(m_swsLaneKey, want)) {
        return true;
    }
    for (SwsContext* lane : m_swsLanes) {
        sws_freeContext(lane);
    }
    m_swsLanes.clear();
    m_swsLaneKey = SwsCacheKey{};
    const AVPixelFormat dstFormat =
        sixteenBit ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_BGRA;
    m_swsLanes.reserve(static_cast<size_t>(lanes));
    for (int i = 0; i < lanes; ++i) {
        SwsContext* lane = sws_getContext(w, h, srcFormat, w, h, dstFormat,
                                          SWS_BILINEAR, nullptr, nullptr,
                                          nullptr);
        if (!lane) break;
        sws_setColorspaceDetails(lane, sws_getCoefficients(p.srcCsp),
                                 p.srcRange, sws_getCoefficients(p.dstCsp),
                                 p.dstRange, 0, 1 << 16, 1 << 16);
        m_swsLanes.push_back(lane);
    }
    if (static_cast<int>(m_swsLanes.size()) != lanes) {
        for (SwsContext* lane : m_swsLanes) {
            sws_freeContext(lane);
        }
        m_swsLanes.clear();
        return false;
    }
    m_swsLaneKey = want;
    return true;
}

bool FfmpegVideoDecoder::TryConvertParallel(AVFrame* srcFrame,
                                            AVPixelFormat frameFormat) {
    const int cap = std::min<int>(
        8,
        std::max<int>(1, static_cast<int>(std::thread::hardware_concurrency())));
    if (cap <= 1 || srcFrame->height < cap * 48) {
        return false;  // small frame: thread spawn cost would dominate
    }
    // Bit-exactness of strips is only proven for sources without vertical
    // chroma subsampling (see SwsParallelStripsAllowed); everything else
    // stays on the single context, where swscale's SIMD converters shine.
    if (!SwsParallelStripsAllowed(frameFormat)) {
        return false;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(frameFormat);
    const auto slices = PlanSwsSlices(srcFrame->height, cap);
    if (slices.size() < 2) return false;
    if (!EnsureSwsLanes(static_cast<int>(slices.size()), srcFrame->width,
                        srcFrame->height, frameFormat, m_hdrContent)) {
        return false;
    }
    const int dstPitch = m_swFrame->linesize[0];
    const int dstStride[4] = {dstPitch, 0, 0, 0};
    std::atomic<bool> allOk{true};
    {
        std::vector<std::thread> workers;
        workers.reserve(slices.size());
        for (size_t i = 0; i < slices.size(); ++i) {
            workers.emplace_back([&, i]() {
                const SwsSlice& sl = slices[i];
                // Strip-as-frame: the pointers are pre-offset to the strip
                // but srcSliceY stays 0, so each call is a legal frame
                // start for its lane context; identity sizing means no
                // filter state crosses strip boundaries.
                uint8_t* src[4] = {};
                SliceSourcePointers(srcFrame->data, srcFrame->linesize, sl.y,
                                    desc, src);
                uint8_t* dst[4] = {
                    m_swFrame->data[0] + static_cast<size_t>(sl.y) * dstPitch,
                    nullptr, nullptr, nullptr};
                const int ret = sws_scale(m_swsLanes[i], src,
                                          srcFrame->linesize, 0, sl.rows,
                                          dst, dstStride);
                if (ret < 0) {
                    allOk.store(false, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread& t : workers) {
            t.join();
        }
    }
    return allOk.load();
}

bool FfmpegVideoDecoder::ConvertSoftware(VideoFrame& outFrame) {
    // Software fallback: swscale to BGRA (SDR) / RGBA64 (HDR), staging
    // upload, CopyResource into the pool slot (CPU round-trip).
    // For HW-decoded HDR: downloads D3D11 frame to CPU first via
    // av_hwframe_transfer_data, then swscale on the software frame.
    if (!m_loggedSwFallback) {
        m_loggedSwFallback = true;
        REX_LOG_WARN(kModule, "software frame conversion (CPU upload) "
                     "active decodePath={}",
                     m_hdrContent ? "SW_RGBA16" : "SW_BGRA8");
    }
    if (!m_swFrame || !m_stream) return false;

    // If the decoded frame is D3D11 (HW accel), download to CPU first.
    AVFrame* srcFrame = m_frame;
    AVFrame* dlFrame = nullptr;
    const bool isD3d11 = (m_frame->format == AV_PIX_FMT_D3D11);
    if (isD3d11) {
        dlFrame = av_frame_alloc();
        if (!dlFrame) return false;
        dlFrame->format = AV_PIX_FMT_NONE;
        if (av_hwframe_transfer_data(dlFrame, m_frame, 0) < 0) {
            REX_LOG_ERROR(kModule, "hwframe download failed for HDR frame");
            av_frame_free(&dlFrame);
            return false;
        }
        srcFrame = dlFrame;
    }

    const auto frameFormat = static_cast<AVPixelFormat>(srcFrame->format);
    const AVPixelFormat swFormat = m_hdrContent ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_BGRA;
    // EnsureSws always applies the resolved matrix/range; contexts left on
    // swscale defaults convert BT.709 sources with the BT.601 matrix
    // (deadpool tint, 2026-10-16). The Open()/SwitchStream presets used to
    // bypass sws_setColorspaceDetails entirely because the old cache check
    // compared only format and bit depth — m_swsKey now carries colorimetry.
    if (!EnsureSws(srcFrame->width, srcFrame->height, frameFormat,
                   m_hdrContent)) {
        REX_LOG_ERROR(kModule, "swscale context creation failed");
        if (dlFrame) av_frame_free(&dlFrame);
        return false;
    }

    if (!EnsurePool(static_cast<UINT>(srcFrame->width),
                     static_cast<UINT>(srcFrame->height),
                     PoolTextureFormat(false /* SW path */, m_hdrContent))) {
        if (dlFrame) av_frame_free(&dlFrame);
        return false;
    }
    // Audit 2026-10-17, §5.2 #4 (cross-listed §5.4 #3): lease through the
    // same consumed-watermark allocator as the GPU path. The old blind
    // round-robin could overwrite a slot the renderer had not consumed yet
    // and never stamped m_slotFrameIndex, so the lease saw stale
    // bookkeeping.
    const UINT slot = PickPoolSlot();
    m_slotFrameIndex[slot] = static_cast<int64_t>(m_frameIndex);

    av_frame_unref(m_swFrame);
    m_swFrame->format = swFormat;
    m_swFrame->width = srcFrame->width;
    m_swFrame->height = srcFrame->height;
    if (av_frame_get_buffer(m_swFrame, 32) < 0) {
        if (dlFrame) av_frame_free(&dlFrame);
        return false;
    }
    // Parallel slice conversion when it can help (FIX B, 4:4:4 10-bit has
    // no SIMD path); the single context above stays the always-correct
    // fallback.
    if (!TryConvertParallel(srcFrame, frameFormat)) {
        sws_scale(m_sws, srcFrame->data, srcFrame->linesize, 0,
                  srcFrame->height, m_swFrame->data, m_swFrame->linesize);
    }

    // Free the download frame now that swscale is done.
    if (dlFrame) av_frame_free(&dlFrame);

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

    // Same shared-fence discipline as the GPU path (audit 2026-10-17 #5).
    const UINT64 fenceValue = SignalSharedFenceOrdered(
        [&](UINT64 v) { m_context4->Signal(m_fence11.Get(), v); });
    m_fenceValue = fenceValue;
    m_slotFence[slot].store(fenceValue, std::memory_order_release);
    m_context->Flush();

    m_cpuToGpuCopies.fetch_add(1, std::memory_order_relaxed);

    outFrame.sharedHandle = m_poolHandles[slot];
    outFrame.slot = slot;
    outFrame.fenceValue = fenceValue;
    outFrame.poolGeneration = m_poolGeneration;
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
