#pragma once

#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

struct SwsContext;  // libswscale (software conversion)

namespace rex::wallpaper {

// ─── VideoColorInfo ─────────────────────────────────────────────────────
//
// Colorimetry of the decoded source, carried on every frame so the renderer
// can do the right thing without probing the decoder: SDR (BT.601/709,
// no EOTF), PQ (ST.2084, BT.2020) or HLG (ARIB STD-B67, BT.2020). HDR
// static metadata (Mastering Display + Content Light Level) is lifted from
// the stream's side data when present.

struct VideoColorInfo {
    enum class Matrix { BT601, BT709, BT2020 };
    enum class Transfer { SDR, PQ, HLG };

    Matrix yuvMatrix = Matrix::BT709;
    Transfer transfer = Transfer::SDR;
    bool fullRange = false;
    int srcWidth = 0;
    int srcHeight = 0;

    // HDR static metadata (in cd/m²; defaults match SDR expectations).
    float masteringDisplayMinLuminance = 0.0f;
    float masteringDisplayMaxLuminance = 1000.0f;
    float maxContentLightLevel = 1000.0f;
    float maxFrameAverageLightLevel = 400.0f;
};

// ─── VideoFrame ─────────────────────────────────────────────────────────
//
// GPU-backed decoded frame. The content lives in the decoder's private
// ring of shareable BGRA textures (pool slot `slot`). The D3D12 consumer
// opens each slot's NT shared handle once (FfmpegVideoDecoder::GetSlotHandle)
// and waits on the slot's D3D12 fence value (GetSlotFence) before sampling;
// both are exposed through VideoManager. Fence values are monotonic, so
// waiting on the latest value of a slot is always race-free.

struct VideoFrame {
    HANDLE   sharedHandle = nullptr;  // NT handle of the pool slot (stable per Open)
    UINT     slot       = 0;      // pool slot index (stable for process lifetime)
    UINT64   fenceValue = 0;      // fence value signaled after the blt into slot
    double   pts        = 0.0;    // presentation timestamp in seconds
    uint64_t frameIndex = 0;      // monotonically increasing decode order
    UINT     width      = 0;
    UINT     height     = 0;
    bool     valid      = false;
    VideoColorInfo colorInfo;     // source colorimetry (HDR metadata when any)
};

// ─── DecoderDiagnostics ─────────────────────────────────────────────────

struct DecoderDiagnostics {
    uint64_t framesDecoded    = 0;  // frames produced by DecodeFrame
    uint64_t gpuToCpuCopies   = 0;  // staging downloads (0 on the GPU path)
    uint64_t cpuToGpuCopies   = 0;  // software-upload conversions
    double   decodeFps        = 0.0;
    double   averageDecodeMs  = 0.0;
    double   lastDecodeMs     = 0.0;
    uint32_t recoveryCount    = 0;
    uint32_t deviceResets     = 0;
    bool     hardwareAccelerated = false;
    bool     eof              = false;
};

// ─── FfmpegVideoDecoder ─────────────────────────────────────────────────
//
// FFmpeg decode (D3D11VA hardware, libswscale software fallback) that
// converts every frame into a private ring of shareable BGRA textures and
// signals a shared D3D11 fence after each conversion.
//
// FFmpeg's decode textures carry D3D11_BIND_DECODER, which D3D11 refuses to
// share (CreateSharedHandle -> DXGI_ERROR_INVALID_CALL even with
// SHARED | SHARED_NTHANDLE), so each frame is copied into our own pool via a
// hardware video processor (GPU-only, no CPU round-trip) and those pool
// textures are what get shared with D3D12.
//
// Threading: DecodeFrame() must be called from exactly one thread (the
// decode thread). All other members are safe to read concurrently.

class FfmpegVideoDecoder {
public:
    FfmpegVideoDecoder();
    ~FfmpegVideoDecoder();

    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    // context11 must be the immediate context of a D3D11 device on the
    // same adapter as the D3D12 device (created by VideoManager with
    // D3D11_CREATE_DEVICE_VIDEO_SUPPORT | BGRA_SUPPORT). The device is
    // recovered from the context (ID3D11Device does not QI into the
    // context). fenceHandle: NT handle of a D3D12-created shared fence,
    // opened here as an ID3D11Fence and signaled after every conversion.
    shared::Result<void> Open(std::string_view path, bool useHardware,
                              ID3D11DeviceContext* context11, HANDLE fenceHandle);
    void Close();
    bool IsOpen() const { return m_open.load(std::memory_order_acquire); }

    /// Decodes one frame. Returns false on EOF (or error); on success
    /// outFrame describes the pool slot the frame was converted into.
    bool DecodeFrame(VideoFrame& outFrame);

    void SetLoop(bool loop) { m_looping.store(loop, std::memory_order_relaxed); }
    bool GetLoop() const { return m_looping.load(std::memory_order_relaxed); }
    bool Seek(double seconds);
    bool Flush();

    /// Picks the video stream closest to (but not above) the target
    /// resolution; if none fits, the smallest stream. Re-opens the codec
    /// on the chosen stream, keeping the current decode session alive.
    /// Must not run concurrently with DecodeFrame() (caller serializes).
    void SelectStreamByResolution(int targetWidth, int targetHeight);

    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }

    // Shared-surface contract for the D3D12 renderer.
    HANDLE GetSlotHandle(UINT slot) const;
    UINT64 GetSlotFence(UINT slot) const;

    /// Colorimetry of the stream as last resolved from a decoded frame.
    const VideoColorInfo& GetColorInfo() const { return m_colorInfo; }

    DecoderDiagnostics GetDiagnostics() const;

private:
    bool SeekInternal(double seconds);
    VideoColorInfo ResolveColorInfo() const;
    bool EnsurePool(UINT width, UINT height);
    bool ConvertAndSignal(VideoFrame& outFrame);
    bool ConvertSoftware(VideoFrame& outFrame);

    // ─── FFmpeg state ───────────────────────────────────────────────────
    AVFormatContext* m_fmtCtx      = nullptr;
    AVCodecContext*  m_decCtx      = nullptr;
    AVBufferRef*     m_hwDevCtx    = nullptr;
    AVFrame*         m_frame       = nullptr;
    AVFrame*         m_swFrame     = nullptr;
    AVPacket*        m_pkt         = nullptr;
    SwsContext*      m_sws         = nullptr;
    int  m_videoStreamIndex        = -1;
    AVStream* m_stream             = nullptr;
    bool m_pktPending              = false;
    bool m_hardware                = false;
    bool m_eof                     = false;
    std::atomic<bool> m_looping{true};
    bool m_loggedSwFallback        = false;

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    int    m_width     = 0;
    int    m_height    = 0;

    // ─── D3D11 state ────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Device>         m_device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  m_context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>    m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>   m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1>  m_videoContext1;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
    Microsoft::WRL::ComPtr<ID3D11Fence>          m_fence11;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>      m_staging;

    // ─── Shareable BGRA pool ────────────────────────────────────────────
    static constexpr UINT kPoolSize = 8;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_poolTex;
    std::vector<HANDLE> m_poolHandles;
    std::atomic<UINT64> m_slotFence[kPoolSize]{};
    UINT m_poolW = 0;
    UINT m_poolH = 0;
    UINT m_poolNext = 0;
    bool m_poolHdr = false;   // pool format is 16-bit float (HDR sources)
    bool m_hdrContent = false; // current stream carries PQ/HLG transfer
    UINT64 m_fenceValue = 0;
    UINT64 m_frameIndex = 0;
    std::atomic<bool> m_open{false};

    // ─── Colorimetry (last resolved from a decoded frame) ───────────────
    VideoColorInfo m_colorInfo;
    bool m_sws16bit = false;  // swscale target is RGBA64 (HDR software path)

    // ─── Telemetry ──────────────────────────────────────────────────────
    mutable std::atomic<uint64_t> m_framesDecoded{0};
    mutable std::atomic<uint64_t> m_cpuToGpuCopies{0};
    mutable std::atomic<double>   m_decodeMsSum{0.0};
    mutable std::atomic<double>   m_lastDecodeMs{0.0};
};

} // namespace rex::wallpaper
