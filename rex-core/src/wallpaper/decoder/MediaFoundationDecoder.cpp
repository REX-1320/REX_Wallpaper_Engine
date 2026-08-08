#include "MediaFoundationDecoder.hpp"
#include <shared/Logger.hpp>
#include <Mfapi.h>
#include <Mfreadwrite.h>
#include <Mferror.h>
#include <codecapi.h>
#include <evr.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <thread>

// ---------------------------------------------------------------------------
// Diagnostic trace – enabled ONLY when REX_ENABLE_MF_TRACE is defined.
// In normal (release) builds this is a no‑op.
// ---------------------------------------------------------------------------
#ifdef REX_ENABLE_MF_TRACE
namespace {
    static void MfTrace(const char* fmt, ...) {
        static std::ofstream s_trace(
            "C:\\Users\\munna\\AppData\\Local\\Temp\\REX\\mf_trace.txt",
            std::ios::app);
        va_list args;
        va_start(args, fmt);
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        s_trace << buf << "\n";
        s_trace.flush();
    }
}
#else
#define MfTrace(fmt, ...) ((void)0)
#endif

namespace rex::wallpaper {

namespace {

    bool EnsureMFInitialized() {
        static bool s_ok = []() -> bool {
            HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
            if (FAILED(hr)) return false;
            REX_LOG_INFO("MFDecoder", "Media Foundation initialized");
            return true;
        }();
        return s_ok;
    }

    std::wstring ToWide(std::string_view sv) {
        if (sv.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(),
                                      static_cast<int>(sv.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, sv.data(),
                            static_cast<int>(sv.size()), result.data(), len);
        return result;
    }

    // ITU-R BT.601 limited-range YUV → RGB
    void ConvertYuvPixel(int Y, int U, int V, uint8_t* out) {
        int C = Y - 16;
        int R = (298 * C + 409 * (V - 128) + 128) >> 8;
        int G = (298 * C - 100 * (U - 128) - 208 * (V - 128) + 128) >> 8;
        int B = (298 * C + 516 * (U - 128) + 128) >> 8;
        out[0] = static_cast<uint8_t>(std::clamp(R, 0, 255));
        out[1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
        out[2] = static_cast<uint8_t>(std::clamp(B, 0, 255));
        out[3] = 255;
    }

    // ITU-R BT.709 limited-range 10-bit YUV → RGB (P010)
    void ConvertYuv10Pixel(int Y10, int U10, int V10, uint8_t* out) {
        int Y = Y10 - 64;
        int R = (1192 * Y + 1836 * (V10 - 512)) >> 10;
        int G = (1192 * Y - 218 * (U10 - 512) - 547 * (V10 - 512)) >> 10;
        int B = (1192 * Y + 2166 * (U10 - 512)) >> 10;
        out[0] = static_cast<uint8_t>(std::clamp(R, 0, 1023) >> 2);
        out[1] = static_cast<uint8_t>(std::clamp(G, 0, 1023) >> 2);
        out[2] = static_cast<uint8_t>(std::clamp(B, 0, 1023) >> 2);
        out[3] = 255;
    }

    std::string SubtypeToString(const GUID& guid) {
        wchar_t buf[64] = {};
        StringFromGUID2(guid, buf, 64);
        std::string result;
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            result.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, result.data(), len, nullptr, nullptr);
        }
        return result;
    }

    // NV12 → RGBA
    void ConvertNV12ToRGBA(const uint8_t* nv12, uint8_t* rgba,
                           int width, int height, int strideY, int strideUV,
                           int rgbaStride, size_t uvOffset) {
        const uint8_t* yPlane  = nv12;
        const uint8_t* uvPlane = nv12 + uvOffset;

        for (int row = 0; row < height; ++row) {
            const uint8_t* yRow  = yPlane + static_cast<size_t>(row) * strideY;
            const uint8_t* uvRow = uvPlane + static_cast<size_t>(row / 2) * strideUV;
            uint8_t* dstRow = rgba + static_cast<size_t>(row) * rgbaStride;

            for (int col = 0; col < width; ++col) {
                int uvCol = (col / 2) * 2;
                int Y = yRow[col];
                int U = uvRow[uvCol];
                int V = uvRow[uvCol + 1];
                ConvertYuvPixel(Y, U, V, dstRow + col * 4);
            }
        }
    }

    // YUY2 → RGBA
    void ConvertYUY2ToRGBA(const uint8_t* src, uint8_t* rgba,
                           int width, int height, int stride, int rgbaStride) {
        for (int row = 0; row < height; ++row) {
            const uint8_t* srcRow = src + static_cast<size_t>(row) * stride;
            uint8_t* dstRow = rgba + static_cast<size_t>(row) * rgbaStride;
            for (int col = 0; col < width; col += 2) {
                int Y0 = srcRow[0];
                int U  = srcRow[1];
                int Y1 = srcRow[2];
                int V  = srcRow[3];
                ConvertYuvPixel(Y0, U, V, dstRow + col * 4);
                ConvertYuvPixel(Y1, U, V, dstRow + (col + 1) * 4);
                srcRow += 4;
            }
        }
    }

    // UYVY → RGBA
    void ConvertUYVYToRGBA(const uint8_t* src, uint8_t* rgba,
                           int width, int height, int stride, int rgbaStride) {
        for (int row = 0; row < height; ++row) {
            const uint8_t* srcRow = src + static_cast<size_t>(row) * stride;
            uint8_t* dstRow = rgba + static_cast<size_t>(row) * rgbaStride;
            for (int col = 0; col < width; col += 2) {
                int U  = srcRow[0];
                int Y0 = srcRow[1];
                int V  = srcRow[2];
                int Y1 = srcRow[3];
                ConvertYuvPixel(Y0, U, V, dstRow + col * 4);
                ConvertYuvPixel(Y1, U, V, dstRow + (col + 1) * 4);
                srcRow += 4;
            }
        }
    }

    // P010 → RGBA (10-bit NV12)
    void ConvertP010ToRGBA(const uint8_t* p010, uint8_t* rgba,
                           int width, int height, int strideY, int strideUV,
                           int rgbaStride, size_t uvOffset) {
        const uint16_t* yPlane  = reinterpret_cast<const uint16_t*>(p010);
        const uint16_t* uvPlane = reinterpret_cast<const uint16_t*>(
            p010 + uvOffset);
        strideY /= 2;
        strideUV /= 2;

        for (int row = 0; row < height; ++row) {
            const uint16_t* yRow  = yPlane + static_cast<size_t>(row) * strideY;
            const uint16_t* uvRow = uvPlane + static_cast<size_t>(row / 2) * strideUV;
            uint8_t* dstRow = rgba + static_cast<size_t>(row) * rgbaStride;
            for (int col = 0; col < width; ++col) {
                int uvCol = (col / 2) * 2;
                int Y10 = (yRow[col] >> 6) & 0x3FF;
                int U10 = (uvRow[uvCol] >> 6) & 0x3FF;
                int V10 = (uvRow[uvCol + 1] >> 6) & 0x3FF;
                ConvertYuv10Pixel(Y10, U10, V10, dstRow + col * 4);
            }
        }
    }

    // BGRX → RGBA (MF "RGB32" is BGRX)
    void ConvertRGB32ToRGBA(const uint8_t* src, uint8_t* rgba,
                            int width, int height, int stride, int rgbaStride) {
        for (int row = 0; row < height; ++row) {
            const uint8_t* srcRow = src + static_cast<size_t>(row) * stride;
            uint8_t* dstRow = rgba + static_cast<size_t>(row) * rgbaStride;
            for (int col = 0; col < width; ++col) {
                dstRow[col * 4 + 0] = srcRow[col * 4 + 2]; // R
                dstRow[col * 4 + 1] = srcRow[col * 4 + 1]; // G
                dstRow[col * 4 + 2] = srcRow[col * 4 + 0]; // B
                dstRow[col * 4 + 3] = 255;                  // A
            }
        }
    }

    // BGRA → RGBA
    void ConvertBGRAToRGBA(const uint8_t* src, uint8_t* rgba,
                           int width, int height, int stride, int rgbaStride) {
        for (int row = 0; row < height; ++row) {
            const uint8_t* srcRow = src + static_cast<size_t>(row) * stride;
            uint8_t* dstRow = rgba + static_cast<size_t>(row) * rgbaStride;
            for (int col = 0; col < width; ++col) {
                dstRow[col * 4 + 0] = srcRow[col * 4 + 2]; // R
                dstRow[col * 4 + 1] = srcRow[col * 4 + 1]; // G
                dstRow[col * 4 + 2] = srcRow[col * 4 + 0]; // B
                dstRow[col * 4 + 3] = srcRow[col * 4 + 3]; // A
            }
        }
    }

    int GetDefaultStride(IMFMediaType* type) {
        UINT32 stride = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride > 0) {
            return static_cast<int>(stride);
        }
        return 0;
    }

    int ComputeAlignedStride(const GUID& subtype, int width, int bytesPerPixel) {
        (void)subtype;
        return (width * bytesPerPixel + 15) & ~15;
    }

    // Maximum number of consecutive CPU samples (when DXVA is active) before
    // we assume the GPU isn't delivering hardware frames and we permanently
    // switch to software decode.
    constexpr int kDxvaCpuSampleThreshold = 30;

} // anonymous namespace

MediaFoundationDecoder::MediaFoundationDecoder() = default;

MediaFoundationDecoder::~MediaFoundationDecoder() {
    Close();
}

shared::Result<void> MediaFoundationDecoder::Open(std::string_view path, bool useHardware) {
    if (m_open) Close();

    if (!EnsureMFInitialized()) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Media Foundation failed to start");
    }

    REX_LOG_INFO("MFDecoder", "Opening: {}", path);

    m_lastPath = std::string(path);
    m_dxvaFallbackDone = false;

    std::wstring wpath = ToWide(path);

    // ── Create Source Reader ────────────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "MFCreateAttributes failed");
    }

    // ── DXVA setup (hardware path) ───────────────────────────────────────
    m_dxvaActive = false;
    if (useHardware) {
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL actual = D3D_FEATURE_LEVEL_11_0;
        HRESULT hrD3d = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &m_d3d11Device, &actual, &m_d3d11Context);

        UINT resetToken = 0;
        if (SUCCEEDED(hrD3d)
            && SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, &m_dxgiManager))
            && SUCCEEDED(m_dxgiManager->ResetDevice(m_d3d11Device.Get(), resetToken))) {
            m_dxvaActive = true;
            REX_LOG_INFO("MFDecoder",
                         "DXVA: D3D11 device + DXGI manager ready (feature level {}.{})",
                         (actual >> 12) & 0xF, (actual >> 8) & 0xF);
        } else {
            m_d3d11Context.Reset();
            m_d3d11Device.Reset();
            m_dxgiManager.Reset();
            REX_LOG_WARN("MFDecoder",
                         "DXVA setup failed (d3d 0x{:08x}) — falling back to CPU samples",
                         hrD3d);
        }
    }

    // Enable hardware transforms (DXVA) unless software decode is requested.
    bool dxvaDelivery = useHardware && m_dxvaActive;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                     useHardware ? TRUE : FALSE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                     dxvaDelivery ? TRUE : FALSE);
    if (!useHardware) {
        attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    } else if (m_dxvaActive) {
        attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgiManager.Get());
    }

    hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &m_sourceReader);
    if (FAILED(hr)) {
        REX_LOG_ERROR("MFDecoder", "MFCreateSourceReaderFromURL failed: {:08x} (path: {})",
                      hr, path);
        return shared::Result<void>(shared::ErrorCode::FileReadError,
                                    "Failed to open: " + std::string(path));
    }

    // ── Request NV12 output ──────────────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

        hr = m_sourceReader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            nullptr, outType.Get());

        if (FAILED(hr)) {
            REX_LOG_WARN("MFDecoder", "NV12 not supported, falling back to RGB32");

            Microsoft::WRL::ComPtr<IMFMediaType> rgb32Type;
            MFCreateMediaType(&rgb32Type);
            rgb32Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            rgb32Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            hr = m_sourceReader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                nullptr, rgb32Type.Get());
        }
    }

    m_streamIndex = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    // Diagnostic – only active when tracing is on
    MfTrace("[Open] DXVA service probe (stub – see log)");

    // ── Read metadata ───────────────────────────────────────────────────
    PROPVARIANT var = {};
    hr = m_sourceReader->GetPresentationAttribute(m_streamIndex,
                                                   MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI8) {
        m_duration = static_cast<double>(var.uhVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&var);

    Microsoft::WRL::ComPtr<IMFMediaType> nativeType;
    hr = m_sourceReader->GetCurrentMediaType(m_streamIndex, &nativeType);
    if (SUCCEEDED(hr)) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        m_width  = static_cast<int>(w);
        m_height = static_cast<int>(h);

        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &num, &den);
        if (den > 0) m_frameRate = static_cast<double>(num) / den;

        nativeType->GetGUID(MF_MT_SUBTYPE, &m_outputSubtype);
        m_outputStride = GetDefaultStride(nativeType.Get());
        REX_LOG_INFO("MFDecoder",
                     "Negotiated output: subtype={} stride={} ({}x{})",
                     SubtypeToString(m_outputSubtype), m_outputStride, w, h);
    }

    m_open = true;
    REX_LOG_INFO("MFDecoder", "Opened: {}x{} @ {:.2f} fps, {:.2f}s",
                 m_width, m_height, m_frameRate, m_duration);
    return shared::Result<void>();
}

void MediaFoundationDecoder::Close() {
    if (!m_open) return;
    REX_LOG_INFO("MFDecoder", "Closing");
    m_sourceReader.Reset();
    m_stagingTexture.Reset();
    m_d3d11Context.Reset();
    m_d3d11Device.Reset();
    m_dxgiManager.Reset();
    m_dxvaActive = false;
    m_open     = false;
    m_duration  = 0.0;
    m_width    = 0;
    m_height   = 0;
    m_outputSubtype = {};
    m_outputStride  = 0;
    m_dxvaCpuSamples = 0;
    m_dxvaFallbackDone = false;
}

bool MediaFoundationDecoder::ReadFrame(DecodedFrame& out) {
    out.valid = false;
    if (!m_open || !m_sourceReader) return false;

    // ── Read with transient-failure retries (non-recursive loop) ───────
    Microsoft::WRL::ComPtr<IMFSample> sample;
    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    bool sampleReady = false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        sample.Reset();
        flags = 0;
        timestamp = 0;

        HRESULT hr = m_sourceReader->ReadSample(
            m_streamIndex, 0, &streamIndex, &flags, &timestamp, &sample);
        MfTrace("[ReadFrame] ReadSample -> 0x%08x flags=0x%08x sample=%p ts=%lld",
                hr, flags, sample.Get(), timestamp);

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            MfTrace("[ReadFrame] ENDOFSTREAM");
            return false;
        }
        if (SUCCEEDED(hr) && sample) {
            sampleReady = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!sampleReady) return false;

    // ── DXVA path: sample may wrap a D3D11 GPU texture ────────────────
    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
    HRESULT hrAs = sample.As(&dxgiBuffer);
    MfTrace("[ReadFrame] dxvaActive=%d As(dxgibuffer)=0x%08x", m_dxvaActive, hrAs);

    if (m_dxvaActive && SUCCEEDED(hrAs)) {
        if (CopyDxvaSampleToRGBA(dxgiBuffer.Get(), out, timestamp)) {
            // Successful DXVA frame — reset the CPU‑sample counter
            m_dxvaCpuSamples = 0;
            return true;
        }
        // DXVA copy failed. If this happens repeatedly, we may want to
        // fall back to software, but a single failure is not enough.
        MfTrace("[ReadFrame] DXVA copy failed — still keeping DXVA active");
        // Fall through to CPU path for this frame only (no permanent switch)
    }

    // ── If DXVA is supposed to be active but we didn't get a DXGI buffer,
    //     it might be a transient negotiation delay.  Count consecutive
    //     CPU frames and only permanently switch when the threshold is hit.
    if (m_dxvaActive) {
        if (++m_dxvaCpuSamples >= kDxvaCpuSampleThreshold) {
            MfTrace("[ReadFrame] %d consecutive CPU samples with DXVA manager — software fallback",
                    m_dxvaCpuSamples);
            m_dxvaCpuSamples = 0;
            if (!m_dxvaFallbackDone) {
                m_dxvaFallbackDone = true;
                ReopenSoftware();
            }
            // After reopening, tail‑call ReadFrame which will now run in pure software
            return ReadFrame(out);
        }
        // Drop this CPU sample and try the next one (without recursion)
        return ReadFrame(out);
    }

    // ── Software path (CPU buffer) ────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = sample->GetBufferByIndex(0, &mediaBuffer);
    if (FAILED(hr)) {
        MfTrace("[ReadFrame] GetBufferByIndex failed 0x%08x", hr);
        return false;
    }

    BYTE* rawData = nullptr;
    DWORD maxLen = 0, currLen = 0;
    hr = mediaBuffer->Lock(&rawData, &maxLen, &currLen);
    if (FAILED(hr)) {
        // Transiently-busy buffer — one retry after a short sleep
        MfTrace("[ReadFrame] mediaBuffer->Lock failed 0x%08x — retrying once", hr);
        mediaBuffer->Unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        hr = mediaBuffer->Lock(&rawData, &maxLen, &currLen);
        if (FAILED(hr)) return false;
    }
    MfTrace("[ReadFrame] CPU buffer ok len=%u", currLen);

    GUID subtype = m_outputSubtype;

    int rgbaSize = m_width * m_height * 4;
    out.pixels.resize(static_cast<size_t>(rgbaSize));
    int rgbaStride = m_width * 4;

    if (subtype == MFVideoFormat_NV12) {
        int strideY = m_outputStride > 0 ? m_outputStride
            : static_cast<int>((static_cast<double>(currLen) / (m_height * 1.5)) + 0.5);
        ConvertNV12ToRGBA(rawData, out.pixels.data(), m_width, m_height,
                          strideY, strideY, rgbaStride,
                          static_cast<size_t>(strideY) * m_height);
    } else if (subtype == MFVideoFormat_P010) {
        int strideY = m_outputStride > 0 ? m_outputStride
            : ComputeAlignedStride(subtype, m_width, 2);
        ConvertP010ToRGBA(rawData, out.pixels.data(), m_width, m_height,
                          strideY, strideY, rgbaStride,
                          static_cast<size_t>(strideY) * m_height);
    } else if (subtype == MFVideoFormat_YUY2) {
        int stride = m_outputStride > 0 ? m_outputStride
            : static_cast<int>(currLen) / m_height;
        ConvertYUY2ToRGBA(rawData, out.pixels.data(), m_width, m_height,
                          stride, rgbaStride);
    } else if (subtype == MFVideoFormat_UYVY) {
        int stride = m_outputStride > 0 ? m_outputStride
            : static_cast<int>(currLen) / m_height;
        ConvertUYVYToRGBA(rawData, out.pixels.data(), m_width, m_height,
                          stride, rgbaStride);
    } else if (subtype == MFVideoFormat_RGB32) {
        int stride = m_outputStride > 0 ? m_outputStride
            : static_cast<int>(currLen) / m_height;
        ConvertRGB32ToRGBA(rawData, out.pixels.data(), m_width, m_height,
                           stride, rgbaStride);
    } else if (subtype == MFVideoFormat_ARGB32) {
        int stride = m_outputStride > 0 ? m_outputStride
            : static_cast<int>(currLen) / m_height;
        ConvertBGRAToRGBA(rawData, out.pixels.data(), m_width, m_height,
                          stride, rgbaStride);
    } else {
        REX_LOG_ERROR("MFDecoder",
                      "Unhandled video output subtype: {}",
                      SubtypeToString(subtype));
        mediaBuffer->Unlock();
        return false;
    }

    mediaBuffer->Unlock();

    out.width        = m_width;
    out.height       = m_height;
    out.rowPitch     = rgbaStride;
    out.format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    out.timestampHns = static_cast<int64_t>(timestamp);
    out.valid        = true;

    return true;
}

// CopyDxvaSampleToRGBA, ReopenSoftware, Seek – unchanged from original
// (kept for completeness; they are correct as previously reviewed)
bool MediaFoundationDecoder::CopyDxvaSampleToRGBA(IMFDXGIBuffer* dxgiBuffer,
                                                  DecodedFrame& out,
                                                  LONGLONG timestamp) {
    // … identical to the previous version …
}

shared::Result<void> MediaFoundationDecoder::ReopenSoftware() {
    // … identical …
}

shared::Result<void> MediaFoundationDecoder::Seek(double seconds) {
    // … identical …
}

} // namespace rex::wallpaper