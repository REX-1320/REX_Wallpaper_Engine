#include "MediaFoundationDecoder.hpp"
#include <shared/Logger.hpp>
#include <Mfapi.h>
#include <Mfreadwrite.h>
#include <Mferror.h>
#include <codecapi.h>

#include <algorithm>


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

    // Simple NV12→RGBA conversion. NV12 layout:
    //   Y plane:  width * height bytes (1 byte per pixel, luminance)
    //   UV plane: (width/2) * (height/2) * 2 bytes (interleaved U,V)
    void ConvertNV12ToRGBA(const uint8_t* nv12, uint8_t* rgba,
                           int width, int height, int rgbaStride) {
        const uint8_t* yPlane  = nv12;
        const uint8_t* uvPlane = nv12 + (width * height);

        for (int row = 0; row < height; ++row) {
            uint8_t* dstRow = rgba + row * rgbaStride;

            for (int col = 0; col < width; ++col) {
                int uvRow = row / 2;
                int uvCol = (col / 2) * 2;

                int Y = yPlane[row * width + col];
                int U = uvPlane[uvRow * width + uvCol] - 128;
                int V = uvPlane[uvRow * width + uvCol + 1] - 128;

                // ITU-R BT.601 limited-range → full-range RGB
                int C = Y - 16;
                int R = (298 * C + 409 * V + 128) >> 8;
                int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
                int B = (298 * C + 516 * U + 128) >> 8;

                dstRow[col * 4 + 0] = static_cast<uint8_t>(std::clamp(R, 0, 255));
                dstRow[col * 4 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
                dstRow[col * 4 + 2] = static_cast<uint8_t>(std::clamp(B, 0, 255));
                dstRow[col * 4 + 3] = 255;
            }
        }
    }

} // anonymous namespace

MediaFoundationDecoder::MediaFoundationDecoder() = default;

MediaFoundationDecoder::~MediaFoundationDecoder() {
    Close();
}

shared::Result<void> MediaFoundationDecoder::Open(std::string_view path) {
    if (m_open) Close();

    if (!EnsureMFInitialized()) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Media Foundation failed to start");
    }

    REX_LOG_INFO("MFDecoder", "Opening: {}", path);

    std::wstring wpath = ToWide(path);

    // ── Create Source Reader ────────────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "MFCreateAttributes failed");
    }

    // Enable hardware transforms (DXVA)
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &m_sourceReader);
    if (FAILED(hr)) {
        REX_LOG_ERROR("MFDecoder", "MFCreateSourceReaderFromURL failed: {:08x} (path: {})",
                      hr, path);
        return shared::Result<void>(shared::ErrorCode::FileReadError,
                                    "Failed to open: " + std::string(path));
    }

    // ── Request RGB32 output ────────────────────────────────────────────
    // This lets MF handle all format conversion (NV12/YUY2 → RGB) internally
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

        hr = m_sourceReader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            nullptr, outType.Get());

        if (FAILED(hr)) {
            // RGB32 might not be supported by the decoder's output.
            // Fall back to NV12 and convert manually.
            REX_LOG_WARN("MFDecoder", "RGB32 not supported, falling back to NV12 + CPU convert");

            Microsoft::WRL::ComPtr<IMFMediaType> nv12Type;
            MFCreateMediaType(&nv12Type);
            nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            hr = m_sourceReader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                nullptr, nv12Type.Get());
        }
    }

    m_streamIndex = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

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
    }

    // Pre-allocate RGBA buffer
    m_rgbaBuffer.resize(m_width * m_height * 4);

    m_open = true;
    REX_LOG_INFO("MFDecoder", "Opened: {}x{} @ {:.2f} fps, {:.2f}s",
                 m_width, m_height, m_frameRate, m_duration);
    return shared::Result<void>();
}

void MediaFoundationDecoder::Close() {
    if (!m_open) return;
    REX_LOG_INFO("MFDecoder", "Closing");
    m_sourceReader.Reset();
    m_open     = false;
    m_duration  = 0.0;
    m_width    = 0;
    m_height   = 0;
    m_rgbaBuffer.clear();
}

VideoFrame MediaFoundationDecoder::ReadFrame() {
    VideoFrame frame;

    if (!m_open || !m_sourceReader) return frame;

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;

    HRESULT hr = m_sourceReader->ReadSample(
        m_streamIndex, 0, &streamIndex, &flags, &timestamp, &sample);

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        REX_LOG_DEBUG("MFDecoder", "EOF");
        return frame; // valid=false signals caller to loop
    }

    if (FAILED(hr) || !sample) return frame;

    // ── Extract buffer from sample ───────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    hr = sample->GetBufferByIndex(0, &mediaBuffer);
    if (FAILED(hr)) return frame;

    BYTE* rawData = nullptr;
    DWORD maxLen = 0, currLen = 0;
    hr = mediaBuffer->Lock(&rawData, &maxLen, &currLen);
    if (FAILED(hr)) return frame;

    // Check the actual subtype to determine if we need conversion
    Microsoft::WRL::ComPtr<IMFMediaType> actualType;
    hr = m_sourceReader->GetCurrentMediaType(m_streamIndex, &actualType);

    GUID subtype = {};
    bool isNV12 = false;
    if (SUCCEEDED(hr)) {
        actualType->GetGUID(MF_MT_SUBTYPE, &subtype);
        isNV12 = (subtype == MFVideoFormat_NV12);
    }

    int rgbaSize = m_width * m_height * 4;
    if (m_rgbaBuffer.size() < static_cast<size_t>(rgbaSize)) {
        m_rgbaBuffer.resize(rgbaSize);
    }

    if (isNV12) {
        ConvertNV12ToRGBA(rawData, m_rgbaBuffer.data(), m_width, m_height, m_width * 4);
    } else if (subtype == MFVideoFormat_RGB32) {
        // RGB32 from MF is actually BGRX (or BGRA depending on the decoder)
        // We need to ensure RGBA order. RGB32 layout: [B][G][R][X]
        int stride = static_cast<int>(currLen) / m_height;
        for (int row = 0; row < m_height; ++row) {
            const uint8_t* src = rawData + row * stride;
            uint8_t* dst = m_rgbaBuffer.data() + row * m_width * 4;
            for (int col = 0; col < m_width; ++col) {
                dst[col * 4 + 0] = src[col * 4 + 2]; // R
                dst[col * 4 + 1] = src[col * 4 + 1]; // G
                dst[col * 4 + 2] = src[col * 4 + 0]; // B
                dst[col * 4 + 3] = 255;               // A
            }
        }
    } else {
        // Try raw copy (might be wrong byte order, but better than nothing)
        memcpy(m_rgbaBuffer.data(), rawData, std::min(static_cast<DWORD>(rgbaSize), currLen));
    }

    mediaBuffer->Unlock();

    frame.pixels       = m_rgbaBuffer;
    frame.width        = m_width;
    frame.height       = m_height;
    frame.rowPitch     = m_width * 4;
    frame.format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    frame.timestampHns = static_cast<int64_t>(timestamp);
    frame.valid        = true;

    return frame;
}

shared::Result<void> MediaFoundationDecoder::Seek(double seconds) {
    if (!m_open || !m_sourceReader) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
                                    "No media file open");
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10000000.0);

    HRESULT hr = m_sourceReader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    if (FAILED(hr)) {
        REX_LOG_WARN("MFDecoder", "Seek to {}s failed: {:08x}", seconds, hr);
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Seek failed");
    }
    return shared::Result<void>();
}

} // namespace rex::wallpaper
