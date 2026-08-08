#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare — dxgiformat.h is lightweight and has no D3D linkage.
// This is included for DXGI_FORMAT in VideoFrame metadata.
#include <dxgiformat.h>

namespace rex::wallpaper {

// ─── DecodedFrame (CPU-backed RGBA8) ────────────────────────────────────────
//
// Canonical decoded frame used across the wallpaper pipeline (decoder →
// playback ring buffer → renderer). Metadata fields (format, timestampHns)
// are preserved so downstream consumers can handle HDR, tone-mapping, or
// audio sync without re-probing the source stream.
//
// Memory contract: `pixels` is pre-reserved by the playback controller's
// ring slots. The decoder writes directly into the slot buffer and never
// allocates per frame, keeping steady-state memory flat.

struct DecodedFrame {
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

    shared::Result<void> Open(std::string_view path, bool useHardware = true);
    void Close();

    /// Decodes the next frame directly into `out` (its pixel buffer must
    /// be pre-reserved; it is resized to fit if needed). Returns true on
    /// success, false at EOF or on failure — on failure `out.valid` is
    /// cleared. The caller is responsible for looping (seeking to 0).
    bool ReadFrame(DecodedFrame& out);

    shared::Result<void> Seek(double seconds);

    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }
    bool   IsOpen()       const { return m_open; }

private:
    /// Copies a DXGI-backed sample (D3D11 GPU texture) into `out` via a
    /// staging texture + CPU readback. Used when DXVA is active.
    bool CopyDxvaSampleToRGBA(IMFDXGIBuffer* dxgiBuffer, DecodedFrame& out,
                              LONGLONG timestamp);

    /// Reopens the current file in pure software mode (no D3D manager,
    /// MF_SOURCE_READER_DISABLE_DXVA). Used once when DXVA turns out to be
    /// unavailable or unstable (driver crashes, lock failures, samples that
    /// never materialize as DXGI buffers). Safe to call from the decoder
    /// thread: it only resets COM state owned by this decoder.
    shared::Result<void> ReopenSoftware();

    Microsoft::WRL::ComPtr<IMFSourceReader> m_sourceReader;

    // DXVA (hardware decode) state. The D3D11 device exists solely to let
    // Media Foundation allocate decoder surfaces in VRAM; all rendering
    // stays on D3D12.
    Microsoft::WRL::ComPtr<ID3D11Device>          m_d3d11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>   m_d3d11Context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager>  m_dxgiManager;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>       m_stagingTexture;
    UINT  m_stagingWidth  = 0;
    UINT  m_stagingHeight = 0;
    DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;
    bool  m_dxvaActive    = false;
    bool  m_dxvaFallbackDone = false;
    int   m_dxvaCpuSamples = 0;
    int   m_retryDepth    = 0;

    bool   m_open       = false;
    double m_duration    = 0.0;
    double m_frameRate   = 30.0;
    int    m_width       = 0;
    int    m_height      = 0;
    DWORD  m_streamIndex = 0;
    GUID   m_outputSubtype = {};
    int    m_outputStride  = 0;
    std::string m_lastPath;
    /// >0: ask the reader (via AVP) for output scaled to this height.
    /// Shrinks the decoder's CPU sample pool (large for 4K60 software
    /// decode). 0 = native resolution.
    int    m_maxDecodeHeight = 0;
};

} // namespace rex::wallpaper
