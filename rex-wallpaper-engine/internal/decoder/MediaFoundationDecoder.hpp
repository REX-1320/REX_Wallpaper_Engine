#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

// Forward-declare — dxgiformat.h is lightweight and has no D3D linkage.
// This is included for DXGI_FORMAT in VideoFrame metadata.
#include <dxgiformat.h>

namespace rex::wallpaper {

// ─── VideoFrame (CPU-backed RGBA8) ─────────────────────────────────────────
//
// Metadata fields (format, timestampHns) are preserved through the decode
// pipeline so that downstream consumers can handle HDR, tone-mapping,
// or audio sync without re-probing the source stream.

struct VideoFrame {
    std::vector<uint8_t> pixels;
    int         width       = 0;
    int         height      = 0;
    int         rowPitch    = 0;
    DXGI_FORMAT format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    int64_t     timestampHns = 0;  // 100-ns units (same as MF)
    bool        valid       = false;
};

// ─── MediaFoundationDecoder ─────────────────────────────────────────────────
//
// Opens a media file and decodes video frames via Media Foundation.
// MF uses DXVA (hardware decode) internally when available.
// Output frames are converted to RGBA8 on the CPU for simplicity,
// avoiding the complexity of D3D11↔D3D12 texture sharing.
//
// CPU cost of RGBA conversion: ~2-3 ms per 1080p30 frame on modern hardware,
// well within the <5% CPU budget.

class MediaFoundationDecoder {
public:
    MediaFoundationDecoder();
    ~MediaFoundationDecoder();

    shared::Result<void> Open(std::string_view path);
    void Close();

    /// Read next frame. Returns valid=false at EOF.
    /// The caller is responsible for looping (seeking to 0).
    VideoFrame ReadFrame();

    shared::Result<void> Seek(double seconds);

    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }
    bool   IsOpen()       const { return m_open; }

private:
    Microsoft::WRL::ComPtr<IMFSourceReader> m_sourceReader;

    bool   m_open       = false;
    double m_duration    = 0.0;
    double m_frameRate   = 30.0;
    int    m_width       = 0;
    int    m_height      = 0;
    DWORD  m_streamIndex = 0;

    // Pre-allocated RGBA8 conversion buffer
    std::vector<uint8_t> m_rgbaBuffer;
};

} // namespace rex::wallpaper
