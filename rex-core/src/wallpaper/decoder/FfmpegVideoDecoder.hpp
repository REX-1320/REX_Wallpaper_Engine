#pragma once

#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
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
    // P13 per-stage percentiles over the last 128 frames. "decode" spans
    // avcodec_receive_frame through pool publish; "vp" is VideoProcessorBlt
    // alone (including its HDR-fallback retry when one occurs).
    double   decodeMsP50      = 0.0;
    double   decodeMsP95      = 0.0;
    double   decodeMsP99      = 0.0;
    double   vpMsP50          = 0.0;
    double   vpMsP95          = 0.0;
    double   vpMsP99          = 0.0;
    uint32_t recoveryCount    = 0;
    uint32_t deviceResets     = 0;
    // Packets abandoned because avcodec_send_packet stayed at EAGAIN for the
    // whole retry budget. Zero is the healthy state; non-zero means the
    // decoder refused input and a frame may have been lost.
    uint64_t sendEagainStalls = 0;
    // Driver-reported hardware decoder profile count, from
    // ID3D11VideoDevice::GetVideoDecoderProfileCount(). 0 means the device
    // advertises no hardware decode at all, so REX should say so up front
    // instead of discovering it by attempting an open and failing.
    uint32_t hwDecoderProfiles = 0;
    bool     hardwareAccelerated = false;
    bool     eof              = false;
    // P2-1 unusable-path latch: zero frames decoded after the zero-progress
    // budget (or a send-fail streak on a never-decoded path). The supervisor
    // poison-parks on this instead of stalling forever; the reason lives in
    // runtime.log (logged once at latch time), the bool travels in telemetry.
    bool     pathUnusable     = false;
};

// ─── Send-EAGAIN retry policy (pure, unit-testable) ──────────────────────
//
// FFmpeg's contract: avcodec_send_packet returning AVERROR(EAGAIN) means the
// packet was NOT consumed and the caller must keep it and retry — unreffing it
// punches a permanent hole in the compressed stream.
//
// D3D11VA breaks the contract's convenience assumption though: once its surface
// pool saturates, send_packet AND receive_frame can both return EAGAIN, so an
// unbounded retry spins forever while holding the decode mutex (proven live
// 2026-10-11: position froze at 2.45 s). Hence a bounded budget with a
// last-resort advance.
//
// retryCount is the number of consecutive EAGAIN retries already spent on the
// CURRENT packet; maxBudget < 0 means "unbounded" (used by tests to show the
// contract's ideal behaviour).
inline bool ShouldRetrySendPacket(int retryCount, int maxBudget) {
    return retryCount < maxBudget;
}

// ─── Per-codec hardware-decode probe (pure, unit-testable) ─────────────
//
// GetVideoDecoderProfileCount tells REX the driver decodes *something*,
// not whether it decodes *this file's codec* — so today every file attempts
// a D3D11VA open and discovers the answer from avcodec_open2's failure
// (slow opens, warning noise, and a doomed attempt on drivers without the
// codec, e.g. AV1 on pre-RDNA3 AMD). Enumerating the profile GUIDs and
// matching the file's codec skips doomed attempts up front.
//
// UnknownCodec means REX carries no GUIDs for this codec: the caller must
// attempt hardware decode exactly as before (status quo — never refuse
// hardware on an uncertain mapping; the failure direction here is always
// "try", never "skip").
enum class HwProfileSupport { UnknownCodec, Supported, Unsupported };

namespace detail {
inline bool GuidEq(const GUID& a, const GUID& b) {
    if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b.Data4[i]) return false;
    }
    return true;
}

// Well-known DXVA2/D3D11 decoder-profile GUIDs (VLD variants; drivers also
// report IDCT/MoComp ones which REX ignores — VLD-or-nothing is the rule).
inline constexpr GUID kDxvaH264Vld = {
    0x1b81be68, 0xa0c7, 0x11d3,
    {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
inline constexpr GUID kDxvaH264VldStereoProgressive = {
    0xd79e87b7, 0x7eeb, 0x11df,
    {0x9a, 0xe9, 0x22, 0x4e, 0xdf, 0xd7, 0x20, 0x85}};
inline constexpr GUID kDxvaHevcVldMain = {
    0x5b11d51b, 0x2f4c, 0x4452,
    {0xbc, 0x6c, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
inline constexpr GUID kDxvaHevcVldMain10 = {
    0x107af0e0, 0xef1a, 0x4d19,
    {0xab, 0xb8, 0x67, 0xa1, 0x63, 0x49, 0x7c, 0x4c}};
inline constexpr GUID kDxvaVp9VldProfile0 = {
    0x463707f8, 0xa1d0, 0x4585,
    {0x87, 0x6d, 0x83, 0xc2, 0x05, 0xaa, 0x63, 0xf0}};
inline constexpr GUID kDxvaVp9VldProfile2 = {
    0xa4c749ef, 0x6ecf, 0x48aa,
    {0x84, 0x48, 0x50, 0xa6, 0xa9, 0xe5, 0x5d, 0x9a}};
inline constexpr GUID kDxvaAv1VldProfile0 = {
    0xb8be4ccb, 0xcf53, 0x46c0,
    {0x8d, 0x59, 0xd6, 0xb7, 0x4a, 0x91, 0x70, 0xbd}};
inline constexpr GUID kDxvaMpeg2Vld = {
    0xee27417f, 0x5e77, 0x4d14,
    {0xa5, 0x3b, 0x7d, 0xc2, 0xed, 0x57, 0x1f, 0x00}};
inline constexpr GUID kDxvaVc1Vld = {
    0x1b81be94, 0xa0c7, 0x11d3,
    {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};

struct CodecProfiles {
    AVCodecID codec;
    const GUID* guids;
    size_t count;
};
inline constexpr GUID kH264Guids[] = {kDxvaH264Vld,
                                      kDxvaH264VldStereoProgressive};
inline constexpr GUID kHevcGuids[] = {kDxvaHevcVldMain, kDxvaHevcVldMain10};
inline constexpr GUID kVp9Guids[] = {kDxvaVp9VldProfile0, kDxvaVp9VldProfile2};
inline constexpr GUID kAv1Guids[] = {kDxvaAv1VldProfile0};
inline constexpr GUID kMpeg2Guids[] = {kDxvaMpeg2Vld};
inline constexpr GUID kVc1Guids[] = {kDxvaVc1Vld};
inline constexpr CodecProfiles kCodecProfileTable[] = {
    {AV_CODEC_ID_H264, kH264Guids, 2}, {AV_CODEC_ID_HEVC, kHevcGuids, 2},
    {AV_CODEC_ID_VP9, kVp9Guids, 2},   {AV_CODEC_ID_AV1, kAv1Guids, 1},
    {AV_CODEC_ID_MPEG2VIDEO, kMpeg2Guids, 1},
    {AV_CODEC_ID_VC1, kVc1Guids, 1},
};
}  // namespace detail

inline HwProfileSupport CheckHwProfileSupport(AVCodecID codec,
                                              const GUID* profiles,
                                              size_t count) {
    const detail::CodecProfiles* entry = nullptr;
    for (const auto& e : detail::kCodecProfileTable) {
        if (e.codec == codec) {
            entry = &e;
            break;
        }
    }
    if (!entry) return HwProfileSupport::UnknownCodec;
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < entry->count; ++j) {
            if (detail::GuidEq(profiles[i], entry->guids[j])) {
                return HwProfileSupport::Supported;
            }
        }
    }
    return HwProfileSupport::Unsupported;
}

// ─── Unusable-decode-path predicate (pure, unit-testable) ───────────────
//
// Zero frames decoded after `budget` DecodeFrame attempts (or a send-fail
// streak on a never-decoded path) means the path is unusable, not slow:
// surface it so the supervisor can poison-park the file instead of
// stalling on it forever (P2-1/P2-3). Attempts reset on the first decoded
// frame, so a slow-but-working path never trips this.
inline bool DecodePathUnusable(uint64_t framesDecoded, uint64_t attempts,
                               uint64_t budget = 240) {
    return framesDecoded == 0 && attempts >= budget;
}

// ─── Source frame-rate resolution (pure, unit-testable) ────────────────
//
// The container-declared rate (r_frame_rate) can disagree with the actual
// timestamps. Measured 2026-10-15: marvel-spiderman-miles-morales.1920x1080
// declares r_frame_rate=60/1 but its mp4 stsz holds 1222 video samples over
// 47.554 s = 25.69 fps. Every downstream consumer (decode pacing,
// CadenceController::PresentationTarget, reserve watermarks, the
// scheduler's due-windows) trusts this number, so a 2.3x-overstated rate
// makes decode run 2.3x too fast and fills the ring with future frames that
// no scheduler can play correctly (permanent starve under the fixed
// scheduler's retain policy; ~2.4x content under newest-published fallback).
//
// The measured count (nb_frames / container duration) is the true average
// rate for finite files, so prefer it whenever the container provides both.
// Fall back to the declared rate, then to 30. Bounds [1, 480] reject
// estimated/garbage counts; duration <= 1 s is too short to measure from.
// usedCount reports whether the count (true) or the fallback (false) won.
inline double ResolveFrameRate(int64_t nbFrames, double durationSec,
                               double declaredFps,
                               bool* usedCount = nullptr) {
    if (nbFrames > 0 && durationSec > 1.0) {
        const double measured = static_cast<double>(nbFrames) / durationSec;
        if (measured >= 1.0 && measured <= 480.0) {
            if (usedCount) *usedCount = true;
            return measured;
        }
    }
    if (usedCount) *usedCount = false;
    return declaredFps > 0.0 ? declaredFps : 30.0;
}

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

// P1-10: bounds the EOF→seek→EOF cycle inside one DecodeFrame call. A
// normal loop wrap needs exactly one internal seek per call; a second
// consecutive EOF means the seek produced no frames (0-frame file) —
// DecodeFrame must return instead of hanging Open's synchronous prewarm.
struct EofLoopGuard {
    static constexpr int kMaxInternalSeeks = 1;
    int seeks = 0;
    bool AllowSeek() { return seeks++ < kMaxInternalSeeks; }
};

class FfmpegVideoDecoder {
  public:
    /// P13 timing-ring capacity (also used by diagnostics percentile math).
    static constexpr size_t kTimingRingSize = 128;

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

    /// Abort any blocked FFmpeg call (P1-9 / R14): makes a stuck
    /// av_read_frame (or open/stream-info on a dead share) return
    /// AVERROR_EXIT promptly. Called before joining the decode thread —
    /// Close() would otherwise hang until the FFmpeg timeout.
    void RequestStop();

    /// FFmpeg AVIOInterruptCB entry point — do not call directly. Returns
    /// nonzero once RequestStop()/Close() ran; installed on the format
    /// context in Open() so every blocking demuxer call honors it.
    static int InterruptCallback(void* opaque);

    void SetLoop(bool loop) { m_looping.store(loop, std::memory_order_relaxed); }
    bool GetLoop() const { return m_looping.load(std::memory_order_relaxed); }
    bool Seek(double seconds);
    bool Flush();

    /// Pool texture format for a decode path (P0-3 / R3).
    /// HW frames are YUV (VP output): P010 for HDR, NV12 for SDR.
    /// SW frames are RGB (swscale output): 16-bit RGBA for HDR, BGRA for
    /// SDR. Uploading RGB bytes into a YUV pool corrupts every frame.
    static DXGI_FORMAT PoolTextureFormat(bool hardwarePath, bool hdrContent);

    /// Shareable pool slot count, public for the P1 resource envelope
    /// (ResourcePolicy.hpp mirrors it; MediaBudgetMirrorTest pins equality).
    static constexpr UINT kPoolSize = 8;

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
    bool EnsurePool(UINT width, UINT height, DXGI_FORMAT format);
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
    // Send-EAGAIN instrumentation. m_sendEagainRetries is the per-packet retry
    // counter (decode thread only); m_sendEagainStalls counts packets that
    // exhausted the budget and had to be abandoned. A non-zero stall count is
    // proof the decoder was refusing input, i.e. a saturated surface pool.
    int  m_sendEagainRetries      = 0;
    std::atomic<uint64_t> m_sendEagainStalls{0};
    // Driver hardware-decoder capability, probed once at Open().
    uint32_t m_hwDecoderProfiles = 0;
    // Consecutive DecodeFrame calls that produced nothing before any frame
    // has ever been published. See DecodeFrame's zero-progress guard.
    static constexpr uint32_t kZeroProgressReportAfter = 240;
    uint32_t m_zeroProgressCalls = 0;
    // Consecutive avcodec_send_packet failures. Resets on the first success;
    // at kSendFailReportAfter the decoder is treated as unusable and the
    // error is logged once instead of once per packet.
    uint32_t m_sendFailStreak = 0;
    // True once the streak escalation has been logged; a looping wallpaper
    // re-seeks and restarts the streak, so this prevents re-announcing the
    // same unusable-decoder error on every loop until a send succeeds.
    bool m_sendFailReported = false;
    // P2-1 unusable-path latch (decode-thread written, diagnostics read):
    // 0 = healthy, 1 = zero frames after the zero-progress budget,
    // 2 = send-fail streak on a never-decoded path. Write-once per Open
    // (never cleared except by Close/reopen); the reason is logged once at
    // latch time, the code travels in DecoderDiagnostics for the supervisor.
    std::atomic<uint64_t> m_unusableCode{0};
    // P1-8: written by the decode thread, read unlocked by diagnostics —
    // plain bools were a data race. Relaxed: flags only, no ordering.
    std::atomic<bool> m_hardware{false};
    std::atomic<bool> m_eof{false};
    // P1-9: set by RequestStop()/Close(), read by InterruptCallback on
    // FFmpeg's IO thread. Atomic: never take a lock inside the callback.
    std::atomic<bool> m_stopRequested{false};
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
    // (slot count lives in the public kPoolSize above)
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_poolTex;
    std::vector<HANDLE> m_poolHandles;
    std::atomic<UINT64> m_slotFence[kPoolSize]{};
    UINT m_poolW = 0;
    UINT m_poolH = 0;
    UINT m_poolNext = 0;
    // Slot leasing (Phase D prerequisite). A slot must not be reused until the
    // render thread is provably done reading it. The fence only gives GPU
    // ORDERING, not lifetime: after a slot is overwritten, a consumer that
    // already waited on the old fenceValue sees it satisfied and would sample
    // NEW pixels under the OLD VideoFrame identity.
    //
    // So each slot records the decode-order frameIndex it currently holds, and
    // allocation picks the OLDEST slot whose contents are behind the render
    // thread's consumed watermark by at least kSlotReuseMargin frames.
    int64_t m_slotFrameIndex[kPoolSize]{};
    // Highest frameIndex the render thread has finished consuming. Written by
    // the render side (see NotifyFrameConsumed), read by the decode thread.
    std::atomic<int64_t> m_consumedFrameIndex{-1};
    // Keep a slot at least this many frames behind the consumed watermark
    // before reusing it. 3 of 8 slots is conservative and still leaves 5 for
    // the pipeline; it also bounds worst-case staleness if the renderer falls
    // behind (e.g. during a long present stall).
    static constexpr int64_t kSlotReuseMargin = 3;
    // Pool generation key: exact texture format (path + HDR in one).
    // Rebuilt one-time per resolution/format, regardless of HW/SW path.
    DXGI_FORMAT m_poolFormat = DXGI_FORMAT_UNKNOWN;
    bool m_hdrContent = false; // current stream carries PQ/HLG transfer
    UINT64 m_fenceValue = 0;
    UINT64 m_frameIndex = 0;

public:
    /// Render thread reports the frameIndex it has finished consuming. The
    /// decode thread uses it to decide which pool slots are safe to reuse.
    void NotifyFrameConsumed(int64_t frameIndex) {
        m_consumedFrameIndex.store(frameIndex, std::memory_order_release);
    }

private:
    /// Choose the pool slot to write next: the oldest slot that is far enough
    /// behind the consumed watermark to be safely reusable. Falls back to the
    /// strict oldest when nothing satisfies the margin (renderer far behind),
    /// which is the same best-effort behaviour as before but still prefers the
    /// stalest slot instead of blind round-robin.
    UINT PickPoolSlot() const;
    std::atomic<bool> m_open{false};

    // ─── Colorimetry (last resolved from a decoded frame) ───────────────
    VideoColorInfo m_colorInfo;
    bool m_sws16bit = false;  // swscale target is RGBA64 (HDR software path)

    // ─── Telemetry ──────────────────────────────────────────────────────
    mutable std::atomic<uint64_t> m_framesDecoded{0};
    mutable std::atomic<uint64_t> m_cpuToGpuCopies{0};
    mutable std::atomic<double>   m_decodeMsSum{0.0};
    mutable std::atomic<double>   m_lastDecodeMs{0.0};

    // P13 per-stage timing rings (single decode-thread writer; GetDiagnostics
    // is the only reader and tolerates torn/interleaved samples).
    std::array<float, kTimingRingSize> m_decodeMsRing{};
    std::array<float, kTimingRingSize> m_vpMsRing{};
    size_t m_decodeMsCount = 0;   // decode-thread owned
    size_t m_vpMsCount     = 0;   // decode-thread owned

    // P0-1 diagnostics: packets in vs frames out per DecodeFrame call
    // (decode-thread owned; logged env-gated every 600th call). Discriminates
    // "decoder fed 2.4 packets per output frame" from "output reordered".
    uint64_t m_dbgCalls = 0;
    uint64_t m_dbgPackets = 0;  // video packets accepted by send_packet
    uint64_t m_dbgReceives = 0;  // receive_frame successes
};

} // namespace rex::wallpaper
