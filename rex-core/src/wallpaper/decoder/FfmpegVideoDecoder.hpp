#pragma once

#include "wallpaper/playback/StageTimer.hpp"

#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
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
// both are exposed through VideoManager. Fence values are drawn from one
// process-wide monotonic sequence (SignalSharedFenceOrdered), so waiting on
// the latest value of a slot is always race-free.

struct VideoFrame {
    HANDLE   sharedHandle = nullptr;  // NT handle of the pool slot (stable per Open)
    UINT     slot       = 0;      // pool slot index (stable for process lifetime)
    UINT64   fenceValue = 0;      // fence value signaled after the blt into slot
    // Identity of the pool generation that owns `sharedHandle`. NT handle
    // values are recycled by the kernel once closed, so a consumer that caches
    // imported views MUST key them on (handle, generation); matching the
    // handle alone can alias an old import after a pool generation is retired.
    uint64_t poolGeneration = 0;
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
//
// The helper CONSUMES one retry per call and RESETS the count when the budget
// is spent: the budget bounds ONE packet's retries, it must not poison every
// later packet. The defect this guards (audit 2026-10-17 D1): the decoder
// kept its exhausted count across packets, so after a single saturated
// episode — e.g. the startup refill burst loading the decoder — every later
// EAGAIN packet was dropped instantly with no retry at all, which is
// intermittent-start-stutter shaped.
inline bool ShouldRetrySendPacket(int& retryCount, int maxBudget) {
    if (maxBudget < 0 || retryCount < maxBudget) {
        ++retryCount;
        return true;
    }
    retryCount = 0;  // episode over: the NEXT packet gets a fresh budget
    return false;
}

// ─── Unusable-decode-path predicate (pure, unit-testable) ─────────────
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

// ─── Per-codec hardware-decode probe (pure, unit-testable) ─────────────
//
// GetVideoDecoderProfileCount tells REX the driver decodes *something*,
// not whether it decodes *this file's codec* — so without this check every
// file attempts a D3D11VA open and discovers the answer from
// avcodec_open2's failure (slow opens, warning noise, and a doomed attempt
// on drivers without the codec). Enumerating the profile GUIDs and matching
// the file's codec skips doomed attempts up front.
//
// 2026-10-16 CORRECTION: the first version of this table had fabricated
// Data4 tails (remembered prefixes, invented tails), which forced HEVC/VP9
// to software decode with wrong colors. Every constant below is now quoted
// against FFmpeg libavcodec/dxva2.c (ff_DXVA2_Mode*), mingw-w64
// dxva2api.idl, Microsoft Learn "Direct3D 12 video GUIDs" (documents D3D11
// equivalence), and Firefox DXVA2Manager.cpp — all four agree. The canonical
// string form rides in the trailing comment so the bytes stay
// eyeball-verifiable against dxva.h.
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
inline constexpr GUID kDxvaH264Vld = {  // 1b81be68-a0c7-11d3-b984-00c04f2e73c5
    0x1b81be68, 0xa0c7, 0x11d3,
    {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
inline constexpr GUID kDxvaH264VldStereoProgressive = {  // d79e87b7-7eeb-11df-9ae9-224edfd72085
    0xd79e87b7, 0x7eeb, 0x11df,
    {0x9a, 0xe9, 0x22, 0x4e, 0xdf, 0xd7, 0x20, 0x85}};
inline constexpr GUID kDxvaHevcVldMain = {  // 5b11d51b-2f4c-4452-bcc3-09f2a1160cc0
    0x5b11d51b, 0x2f4c, 0x4452,
    {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
inline constexpr GUID kDxvaHevcVldMain10 = {  // 107af0e0-ef1a-4d19-aba8-67a163073d13
    0x107af0e0, 0xef1a, 0x4d19,
    {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}};
inline constexpr GUID kDxvaVp9VldProfile0 = {  // 463707f8-a1d0-4585-876d-83aa6d60b89e
    0x463707f8, 0xa1d0, 0x4585,
    {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}};
inline constexpr GUID kDxvaVp9VldProfile2 = {  // a4c749ef-6ecf-48aa-8448-50a7a1165ff7
    0xa4c749ef, 0x6ecf, 0x48aa,
    {0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7}};
inline constexpr GUID kDxvaAv1VldProfile0 = {  // b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a
    0xb8be4ccb, 0xcf53, 0x46ba,
    {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};
inline constexpr GUID kDxvaMpeg2Vld = {  // ee27417f-5e77-4d14-a53b-7dc2ed571ff0
    0xee27417f, 0x5e77, 0x4d14,
    {0xa5, 0x3b, 0x7d, 0xc2, 0xed, 0x57, 0x1f, 0xf0}};
inline constexpr GUID kDxvaVc1Vld = {  // 1b81be94-a0c7-11d3-b984-00c04f2e73c5
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

// ─── D3D11VA chroma gate (pure, unit-testable) ──────────────────────────
//
// D3D11VA only ever produces 4:2:0 surfaces (NV12/P010). A stream with any
// other chroma layout — most notably HEVC Rext 4:4:4 — still opens fine
// with hwaccel attached, then fails get_format at the first frames and
// FFmpeg silently falls back to SOFTWARE decode while keeping the hardware
// path's single-thread pin (Open() pins thread_count=1 for hwaccel).
// Measured 2026-10-17 on a 3840x1752 HEVC Rext clip: SW decode ran at
// 92-121 ms/frame (~8-11 fps) pinned to one thread vs 21 ms/frame with 8
// threads. Skipping the doomed attempt up front keeps the software path on
// auto threads.
//
// Unknown formats (AV_PIX_FMT_NONE, missing descriptor) keep the status
// quo and attempt hardware: the failure direction is "try", never
// "refuse" (same rule as HwProfileSupport::UnknownCodec).
inline bool D3d11vaCanDecodeChroma(AVPixelFormat fmt) {
    if (fmt == AV_PIX_FMT_NONE) return true;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    if (!d) return true;
    return d->log2_chroma_w == 1 && d->log2_chroma_h == 1;
}

// ─── swscale colorimetry policy (pure, unit-testable) ───────────────────
//
// Every SwsContext creation must apply the source matrix and range;
// contexts left on swscale defaults convert BT.709 sources with the
// BT.601 matrix (deadpool tint, measured 2026-10-16). The cache key
// carries format, bit depth, matrix and range so a context created before
// colorimetry was known can never be reused once it is known.

struct SwsColorParams {
    int srcCsp;
    int srcRange;
    int dstCsp;
    int dstRange;
};

inline SwsColorParams SwsColorParamsFor(VideoColorInfo::Matrix matrix,
                                        bool srcFullRange) {
    const int csp = (matrix == VideoColorInfo::Matrix::BT601)
                        ? SWS_CS_SMPTE170M
                        : (matrix == VideoColorInfo::Matrix::BT2020)
                              ? SWS_CS_BT2020
                              : SWS_CS_ITU709;
    // Destination: full-range RGB in the SOURCE primaries, no gamut
    // conversion. The pixel shader consumes SW-pool bytes as sRGB-encoded
    // full-range RGB (SrgbToLinear, no range expansion).
    return {csp, srcFullRange ? 1 : 0, csp, 1};
}

struct SwsCacheKey {
    AVPixelFormat srcFormat = AV_PIX_FMT_NONE;
    bool sixteenBit = false;
    int csp = 0;
    int srcRange = 0;
    int width = 0;
    int height = 0;
};

inline bool SwsCacheHit(const SwsCacheKey& have, const SwsCacheKey& want) {
    return have.srcFormat == want.srcFormat &&
           have.sixteenBit == want.sixteenBit && have.csp == want.csp &&
           have.srcRange == want.srcRange && have.width == want.width &&
           have.height == want.height;
}

// ─── Parallel conversion strips (FIX B, pure, unit-testable) ────────────
//
// swscale has no SIMD fast path for 4:4:4 10-bit → BGRA (~37-47 ms/frame
// at 4K; the 24 fps budget is 41.7 ms). Conversion is split into one
// horizontal strip per lane, each lane running its own identically
// configured SwsContext over its strip as if it were a whole frame
// (srcSliceY = 0, pre-offset pointers) — swscale rejects mid-frame slice
// starts on fresh contexts ("Slices start in the middle!"), while a strip
// beginning at row 0 is always a legal frame start. Bit-exactness against
// full-frame conversion is required and pinned by the parity tests; it
// holds for sources without vertical chroma subsampling, which is exactly
// the expensive no-SIMD case (see SwsParallelStripsAllowed).

struct SwsSlice {
    int y;     // first luma row (inclusive)
    int rows;  // number of luma rows
};

/// Split `height` luma rows into at most `lanes` strips for parallel
/// conversion. The final strip absorbs the remainder; every strip has
/// >= 1 row.
inline std::vector<SwsSlice> PlanSwsSlices(int height, int lanes) {
    std::vector<SwsSlice> out;
    if (height <= 0) return out;
    if (lanes < 1) lanes = 1;
    if (lanes > height) lanes = height;
    const int base = height / lanes;
    int y = 0;
    for (int i = 0; i + 1 < lanes; ++i) {
        out.push_back(SwsSlice{y, base});
        y += base;
    }
    out.push_back(SwsSlice{y, height - y});
    return out;
}

/// Parallel strips are bit-exact only for sources without VERTICAL chroma
/// subsampling. Measured 2026-10-18 (FFmpeg 8): on 4:2:0 10-bit the two
/// luma rows straddling every strip boundary deviate from the full-frame
/// result, so 4:2:0/4:2:2-style sources keep swscale's fast single-context
/// converters. 4:4:4 10-bit — the format with no SIMD fast path — and
/// 4:2:2 (horizontal-only subsampling) are covered by parity tests.
inline bool SwsParallelStripsAllowed(AVPixelFormat fmt) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    return desc != nullptr && desc->log2_chroma_h == 0;
}

/// Source-plane pointers for one luma-row slice: planes 1/2 (chroma for
/// planar YUV; semi-planar follows the same rule) advance by
/// (sliceY >> log2_chroma_h) rows, plane 0 by sliceY rows.
inline void SliceSourcePointers(uint8_t* const data[4],
                                const int linesize[4], int sliceY,
                                const AVPixFmtDescriptor* desc,
                                uint8_t* out[4]) {
    for (int p = 0; p < 4; ++p) {
        out[p] = nullptr;
        if (!data[p]) continue;
        const int vsub =
            (p == 1 || p == 2) ? (desc ? desc->log2_chroma_h : 0) : 0;
        out[p] = data[p] + static_cast<size_t>(sliceY >> vsub) *
                               static_cast<size_t>(linesize[p]);
    }
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

// ─── Shared fence sequence (pure, unit-testable) ────────────────────────
//
// Every decoder signals ONE process-wide shared fence (VideoManager's
// D3D12-created handle, opened per D3D11 device) and all of them enqueue
// those signals on views of the SAME immediate context. History (audit
// 2026-10-17 #5): each decoder kept a private counter that restarted at 1
// on every Open. Two writers then collide on one fence: a consumer waiting
// on a fresh session's (or the per-monitor decoder's) frame could be
// released by an older producer's higher, stale value before the frame's
// copy had executed, sampling a half-written slot. Values therefore come
// from one process-wide sequence, and the draw + signal enqueue run under
// one lock: enqueue order then equals value order, so the shared fence can
// never move backwards (a signal enqueued after a higher one would strand
// every waiter in between).
inline UINT64 NextSharedFenceValue() {
    static std::atomic<UINT64> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// The lock that keeps a {draw, enqueue} pair atomic across all decoders.
inline std::mutex& SharedFenceLock() {
    static std::mutex lock;
    return lock;
}

/// Draw the next shared-fence value and enqueue its signal in one lock
/// step. `enqueueSignal(value)` must only submit the GPU signal (e.g.
/// ID3D11DeviceContext4::Signal); callers publish the returned value to
/// consumers afterwards.
template <class SignalFn>
inline UINT64 SignalSharedFenceOrdered(SignalFn&& enqueueSignal) {
    std::lock_guard<std::mutex> lock(SharedFenceLock());
    const UINT64 value = NextSharedFenceValue();
    enqueueSignal(value);
    return value;
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
    /// HW frames are YUV decoder surfaces; the pool MUST mirror the actual
    /// surface format (`surfaceFormat`, i.e. desc11.Format) — a mismatched
    /// CopySubresourceRegion is dropped silently and every frame renders as
    /// garbage (the 10-bit SDR "green mess": Main10/bt709 decoding to P010
    /// surfaces while the SDR flag asked for an NV12 pool). `hdrContent` only
    /// selects the SDR/HDR shader math downstream, never the pool format.
    /// Unhandled surface formats fall back to the historical flag choice.
    /// SW frames are RGB (swscale output): 16-bit RGBA for HDR, BGRA for
    /// SDR. Uploading RGB bytes into a YUV pool corrupts every frame.
    static DXGI_FORMAT PoolTextureFormat(
        bool hardwarePath, bool hdrContent,
        DXGI_FORMAT surfaceFormat = DXGI_FORMAT_UNKNOWN);

    /// Slot-leasing allocator (Phase D): the oldest pool slot whose contents
    /// are at least `margin` frames behind the render thread's consumed
    /// watermark, falling back to the strict oldest when nothing qualifies.
    /// Pure integer bookkeeping with no device dependency — public so the
    /// lease tests call this exact function instead of a drift-prone mirror
    /// copy (audit 2026-10-17, §5.2 #15).
    static UINT PickSlotOldestSafe(const int64_t* slotFrameIndex, UINT slots,
                                   int64_t consumed, int64_t margin,
                                   bool haveConsumed);

    /// Pool generation key: exact surface geometry + texture format. The pool
    /// is a ring of shared-handle textures, so a new key means new NT handles,
    /// which means a new import generation for the D3D12 renderer.
    struct PoolGenerationKey {
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    /// Index of a cached generation matching `key`, or `count` on a miss.
    /// Pure lookup, public so the cache tests pin this exact function instead
    /// of a drift-prone mirror (same convention as PickSlotOldestSafe).
    static size_t FindPoolGeneration(const PoolGenerationKey* keys,
                                     size_t count,
                                     const PoolGenerationKey& key);

    /// Index of the least-recently-used generation (smallest usage tick), or
    /// `count` when there is nothing to evict.
    static size_t PickLruPoolGeneration(const uint64_t* useTicks, size_t count);

    /// Live pool generations kept at once: one active plus two retired. Sized
    /// so the common SDR<->HDR (NV12/P010) toggle never rebuilds; a third
    /// distinct geometry/format evicts the oldest retired generation.
    static constexpr size_t kMaxLivePoolGenerations = 3;

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
    /// Stream start timestamp (seconds, 0 for a normal file). The frame PTS
    /// are absolute, so the content spans [start, start + duration].
    double GetStartTime() const { return m_startTime; }
    /// True once the video elementary stream has been decoded to its end
    /// (the delayed frames were flushed). A file's container duration can run
    /// PAST the last video frame - a trailer's audio tail is commonly 1-2 s
    /// longer - so the PTS-vs-container-duration check never sees the end of
    /// the video. The controller latches ENDED from this fact instead.
    bool ReachedEndOfStream() const {
        return m_reachedEnd.load(std::memory_order_acquire);
    }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }

    // Shared-surface contract for the D3D12 renderer.
    HANDLE GetSlotHandle(UINT slot) const;
    UINT64 GetSlotFence(UINT slot) const;

    /// Colorimetry of the stream as last resolved from a decoded frame.
    const VideoColorInfo& GetColorInfo() const { return m_colorInfo; }

    DecoderDiagnostics GetDiagnostics() const;

    /// Stamp the scheduler queue depth onto the most recently pushed stage
    /// sample (-1 = unknown). Called by the controller right after a
    /// successful DecodeFrame, same thread — best-effort diagnostic only.
    void StampStageQueueDepth(int64_t depth);

    /// Snapshot the decode stage ring (oldest-first). Tolerates a concurrent
    /// decode-thread Push the same way GetDiagnostics tolerates the P13
    /// rings (torn samples possible, never a crash).
    std::vector<DecodeStage> SnapshotStages() const;

    /// Write the stage ring to %TEMP%\Paiint\perframe_decode.csv (overwrite).
    /// No-op unless REX_PERFRAME_CSV=1. Returns rows written.
    size_t DumpDecodeStages() const;

private:
    bool SeekInternal(double seconds);
    VideoColorInfo ResolveColorInfo() const;
    /// The hardware decode format callback (C2).
    ///
    /// FFmpeg calls this while opening a hardware decoder, with the list of
    /// formats the decoder can produce. It is the engine's chance to
    /// pre-size the hardware frames: an AVHWFramesContext with the known
    /// width/height lets FFmpeg allocate the decoded surfaces ONCE, in the
    /// right format and size, instead of re-allocating them whenever the
    /// resolution changes mid-stream (which is what forced the pool
    /// rebuilds). Returning a hardware format here also means FFmpeg has
    /// already probed the driver for it - if the driver cannot deliver
    /// D3D11 frames, the callback is never reached and FFmpeg falls back
    /// to software decode on its own.
    static enum AVPixelFormat GetHwFormat(
        AVCodecContext* ctx, const enum AVPixelFormat* const formats);
    bool EnsurePool(UINT width, UINT height, DXGI_FORMAT format);
    /// Create/reuse the software-conversion context. ALWAYS applies the
    /// resolved source matrix/range via sws_setColorspaceDetails; the cache
    /// key includes colorimetry, so a context created before colorimetry
    /// was known (Open/SwitchStream preset) is never reused verbatim.
    bool EnsureSws(int w, int h, AVPixelFormat srcFormat, bool sixteenBit);
    /// Lane contexts for parallel slice conversion (FIX B): `lanes`
    /// contexts, all with the same format/colorimetry/size. Same key
    /// discipline as EnsureSws — anything stale is recreated, and any
    /// creation failure frees all lanes and reports false.
    bool EnsureSwsLanes(int lanes, int w, int h, AVPixelFormat srcFormat,
                        bool sixteenBit);
    /// Convert via PlanSwsSlices lanes; false = caller falls back to the
    /// single-context sws_scale (correct, slower).
    bool TryConvertParallel(AVFrame* srcFrame, AVPixelFormat frameFormat);
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
    // Lane contexts for parallel slice conversion (FIX B). Same cache-key
    // discipline as m_sws: format + bit depth + colorimetry + size.
    std::vector<SwsContext*> m_swsLanes;
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
    // Set when DecodeFrame() has exhausted the video stream (the second
    // AVERROR_EOF, after avcodec_send_packet(nullptr) flushed the delayed
    // frames). Sticky across the internal loop rewind so the controller can
    // observe it after DecodeFrame() returns the first frame of the next
    // pass; reset by a public Seek()/Stop()/Open().
    std::atomic<bool> m_reachedEnd{false};
    // P1-9: set by RequestStop()/Close(), read by InterruptCallback on
    // FFmpeg's IO thread. Atomic: never take a lock inside the callback.
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_looping{true};
    bool m_loggedSwFallback        = false;

    // ─── Codec-context exclusion ───────────────────────────────────────
    //
    // Every call that touches the FFmpeg codec context must be serialised:
    // avcodec_receive_frame / avcodec_send_packet / avcodec_flush_buffers /
    // avcodec_free_context. The software path runs a FRAME-threaded decoder
    // (thread_count = 0 => FFmpeg spawns frame workers), and that machinery
    // keeps an internal async lock: avcodec_flush_buffers() and
    // avcodec_free_context() park the workers by RELEASING that lock, while
    // avcodec_receive_frame() releases it too while it runs. A flush or a
    // close that overlaps a receive therefore releases it twice and trips
    // FFmpeg's av_assert0(fctx->async_lock)
    // (libavcodec/pthread_frame.c, async_unlock) - an ABORT that kills the
    // whole wallpaper runtime. The same overlap is a use-after-free on the
    // context itself.
    //
    // It was reachable: VideoPlaybackController::Close() calls
    // decoder->Close() (which frees the context) OUTSIDE m_decodeMutex,
    // while a concurrent Seek() -> avcodec_flush_buffers() runs under it.
    //
    // Recursive because DecodeFrame() calls SeekInternal() for the EOF loop
    // wrap while already holding it.
    mutable std::recursive_mutex m_codecMutex;

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    // Stream start timestamp (seconds). The frame PTS are absolute stream
    // timestamps, so the content spans [startTime, startTime + duration] -
    // a file with a non-zero start_time would otherwise look like it ended
    // before it began.
    double m_startTime = 0.0;
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
    //
    // The pool generation survives Close() so the next Open can reuse the
    // exact same textures and NT handles: the D3D12 renderer then keeps its
    // existing imports instead of opening the handles again. That matters
    // on AMD, whose driver charges a permanent dedicated-VRAM footprint per
    // imported shared-texture generation (measured: one full 4K pool per
    // OpenSharedHandle cycle, freed only at process exit). ReleasePool() is
    // the explicit teardown, called by the destructor.
    void ReleasePool();

    // ─── Retired pool generations ───────────────────────────────────────
    // A retired generation keeps its textures and NT handles alive so a later
    // switch back to the same geometry/format reuses them instead of forcing
    // the renderer to import a brand-new handle generation. That matters on
    // AMD, whose driver charges a permanent dedicated-VRAM footprint per
    // imported generation (measured 2026-10-28: +264 MB per SDR<->HDR switch,
    // never returned; see ReleasePool). The cache is bounded to
    // kMaxLivePoolGenerations - 1 entries; the oldest is destroyed on overflow.
    struct CachedPool {
        std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> tex;
        std::vector<HANDLE> handles;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vpProcessor;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t generation = 0;
        uint64_t useTick = 0;
    };
    /// Activates a cached generation for this key (if any) and returns true.
    bool ReuseCachedPool(UINT width, UINT height, DXGI_FORMAT format);
    /// Moves the active generation into the cache without destroying it.
    void RetireActivePool();
    /// Destroys the least-recently-used retired generations beyond `keep`,
    /// closing their NT handles.
    void TrimPoolCache(size_t keep);
    std::vector<CachedPool> m_poolCache;
    uint64_t m_poolUseCounter = 0;
    uint64_t m_poolGeneration = 0;      ///< identity of the active generation
    uint64_t m_nextPoolGeneration = 0;

    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_poolTex;
    std::vector<HANDLE> m_poolHandles;
    // Device the current pool generation was created on (identity guard:
    // a video device re-create must not reuse textures from the old device).
    Microsoft::WRL::ComPtr<ID3D11Device> m_poolDevice;
    std::atomic<UINT64> m_slotFence[kPoolSize]{};
    UINT m_poolW = 0;
    UINT m_poolH = 0;
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
    // Last decoder surface format announced in the log (per-change logging,
    // not once per process) — see ConvertHardwareFrame.
    DXGI_FORMAT m_loggedSurfaceFormat = DXGI_FORMAT_UNKNOWN;
    UINT m_loggedSurfaceW = 0;
    UINT m_loggedSurfaceH = 0;
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
    // What the current m_sws context was created for: format, bit depth,
    // matrix, range, size. A context whose key differs from the wanted one
    // is recreated with sws_setColorspaceDetails applied. m_swsLaneKey is
    // the same for the parallel lane contexts (m_swsLanes).
    SwsCacheKey m_swsKey;
    SwsCacheKey m_swsLaneKey;

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

    // Per-frame stage ring (StageTimer.hpp): demux/send/recv/convert split
    // per produced frame. Same single-writer discipline as the P13 rings;
    // DumpDecodeStages() snapshots it to perframe_decode.csv when
    // REX_PERFRAME_CSV=1. queueDepth is stamped by the controller after
    // DecodeFrame returns (-1 = unknown).
    // 2048 rows covers a full ~25-35 s run at 60-90 decode fps, so one dump
    // (5 s cadence, overwrite) spans startup through the first loop wrap.
    StageRing<DecodeStage, 2048> m_stageRing;  // decode-thread owned
    uint64_t m_stageSeq = 0;                  // decode-thread owned
    std::chrono::steady_clock::time_point m_lastProduce{};  // decode-thread
    bool m_haveLastProduce = false;                         // owned

    // P0-1 diagnostics: packets in vs frames out per DecodeFrame call
    // (decode-thread owned; logged env-gated every 600th call). Discriminates
    // "decoder fed 2.4 packets per output frame" from "output reordered".
    uint64_t m_dbgCalls = 0;
    uint64_t m_dbgPackets = 0;  // video packets accepted by send_packet
    uint64_t m_dbgReceives = 0;  // receive_frame successes
};

} // namespace rex::wallpaper
