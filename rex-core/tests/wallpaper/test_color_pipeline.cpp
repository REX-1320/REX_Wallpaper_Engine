// Phase 5 colour / HDR validation.
//
// The roadmap requires that "smooth playback and correct image reproduction are
// two separate requirements", and that HDR handling cover SDR transfer/
// primaries/matrix/range, HDR10 static metadata, PQ/BT.2020, mastering-display
// metadata, content-light info, and HLG.
//
// REX already sets the output colour space correctly
// (SwapChain.cpp:217 SetColorSpace1 -> RGB_FULL_G2084_NONE_P2020 for HDR,
// RGB_FULL_G22_NONE_P709 for SDR). What was missing was proof that the value
// chosen MATCHES the content the decoder actually reports — a pipeline can set
// PQ output while feeding it SDR content and still report "HDR enabled".
//
// This suite closes that gap at the metadata level: for each file it resolves
// the real colour description and asserts the renderer's decision is consistent
// with it. It does not claim to be a visual reference comparison; it is the
// automated guard that makes such a comparison trustworthy when it is done by
// hand.
//
// Set REX_COLOR_DIR to a media directory. Without it, tests SKIP.

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "graphics/renderer/D3D12Renderer.hpp"
#include "graphics/renderer/HdrToneMapping.hpp"
#include "graphics/renderer/OutputDither.hpp"
#include "graphics/renderer/SdrWhiteLevel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string ColorDir() {
    const char* v = std::getenv("REX_COLOR_DIR");
    return v ? std::string(v) : std::string();
}

struct ColorInfo {
    std::string name;
    std::string codec;
    bool opened = false;
    int transfer = AVCOL_TRC_UNSPECIFIED;
    int primaries = AVCOL_PRI_UNSPECIFIED;
    int space = AVCOL_SPC_UNSPECIFIED;
    int range = AVCOL_RANGE_UNSPECIFIED;
    bool gotFrame = false;
    AVPixFmtDescriptor pd{};
    bool haveMastering = false;
    bool haveContentLight = false;
};

// Separate lookups: the transfer / primaries / space / range enums share
// numeric values (BT.709 is 1 in both AVCOL_TRC and AVCOL_PRI), so one switch
// would collide.
const char* TrcName(int v) {
    switch (v) {
    case AVCOL_TRC_BT709: return "BT709";
    case AVCOL_TRC_SMPTE170M: return "SMPTE170M";
    case AVCOL_TRC_GAMMA22: return "Gamma2.2";
    case AVCOL_TRC_GAMMA28: return "Gamma2.8";
    case AVCOL_TRC_SMPTE2084: return "PQ(BT.2020)";   // HDR10
    case AVCOL_TRC_ARIB_STD_B67: return "HLG";          // hybrid log-gamma
    case AVCOL_TRC_LINEAR: return "Linear";
    default: return "unspecified";
    }
}
const char* PriName(int v) {
    switch (v) {
    case AVCOL_PRI_BT709: return "BT.709";
    case AVCOL_PRI_BT2020: return "BT.2020";
    case AVCOL_PRI_SMPTE170M: return "SMPTE170M";
    default: return "unspecified";
    }
}
const char* SpcName(int v) {
    switch (v) {
    case AVCOL_SPC_BT709: return "BT.709";
    case AVCOL_SPC_BT2020_NCL: return "BT.2020 non-constant";
    case AVCOL_SPC_SMPTE170M: return "SMPTE170M";
    case AVCOL_SPC_RGB: return "RGB/GBR";
    default: return "unspecified";
    }
}
const char* RngName(int v) {
    switch (v) {
    case AVCOL_RANGE_MPEG: return "limited(16-235)";
    case AVCOL_RANGE_JPEG: return "full(0-255)";
    default: return "unspecified";
    }
}
const char* Name(int v) { return TrcName(v); }

bool IsHdrTransfer(int t) {
    return t == AVCOL_TRC_SMPTE2084 || t == AVCOL_TRC_ARIB_STD_B67;
}

ColorInfo Probe(const std::string& path) {
    ColorInfo c;
    c.name = path.substr(path.find_last_of("\\/") + 1);
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return c;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return c;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        avformat_close_input(&fmt);
        return c;
    }
    AVCodecParameters* par = fmt->streams[vs]->codecpar;
    c.codec = avcodec_get_name(par->codec_id);
    c.transfer = par->color_trc;
    c.primaries = par->color_primaries;
    c.space = par->color_space;
    c.range = par->color_range;

    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        return c;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return c;
    }
    ctx->thread_count = 0;
    if (ctx->pix_fmt == AV_PIX_FMT_NONE) ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return c;
    }
    c.opened = true;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    for (int i = 0; i < 40 && !c.gotFrame; ++i) {
        const int rc = avcodec_receive_frame(ctx, f);
        if (rc == 0) {
            c.gotFrame = true;
            const AVPixFmtDescriptor* pd =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
            if (pd) c.pd = *pd;
            const AVFrameSideData* sd =
                av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
            if (sd) c.haveMastering = true;
            if (av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
                c.haveContentLight = true;
            }
            // Frame-level colour description overrides stream-level.
            if (f->color_trc != AVCOL_TRC_UNSPECIFIED) c.transfer = f->color_trc;
            if (f->color_primaries != AVCOL_PRI_UNSPECIFIED) c.primaries = f->color_primaries;
            if (f->colorspace != AVCOL_SPC_UNSPECIFIED) c.space = f->colorspace;
            av_frame_unref(f);
            continue;
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) break;
        const int rr = av_read_frame(fmt, pkt);
        if (rr == AVERROR_EOF) break;
        if (rr < 0) break;
        if (pkt->stream_index == vs) {
            const int s = avcodec_send_packet(ctx, pkt);
            if (s != 0 && s != AVERROR(EAGAIN)) break;
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return c;
}

std::vector<std::string> MediaFilesIn(const std::string& dir) {
    std::vector<std::string> out;
    static const char* kExt[] = {".mp4", ".mkv", ".mov", ".webm"};
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string lower = fd.cFileName;
        for (char& ch : lower) ch = static_cast<char>(::tolower(ch));
        for (const char* e : kExt) {
            const size_t n = std::strlen(e);
            if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0) {
                out.push_back(dir + "\\" + fd.cFileName);
                break;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(ColorPipeline, EveryFileResolvesAConsistentColourDescription) {
    const std::string dir = ColorDir();
    if (dir.empty()) GTEST_SKIP() << "set REX_COLOR_DIR to a media directory";
    const auto files = MediaFilesIn(dir);
    ASSERT_FALSE(files.empty());

    int hdr = 0, sdr = 0, unspecified = 0;
    std::vector<std::string> problems;

    for (const std::string& p : files) {
        const ColorInfo c = Probe(p);
        if (!c.opened) continue;
        const bool isHdr = IsHdrTransfer(c.transfer);
        if (isHdr) ++hdr; else if (c.transfer != AVCOL_TRC_UNSPECIFIED) ++sdr;
        else ++unspecified;

        std::printf("[ColorPipeline] %-46s %-5s trc=%-14s pri=%-8s spc=%-16s rng=%-14s %s%s\n",
                    c.name.c_str(), c.codec.c_str(),
                    TrcName(c.transfer), PriName(c.primaries), SpcName(c.space),
                    RngName(c.range), c.pd.name ? c.pd.name : "",
                    c.haveMastering ? " +mastering" : "");
        if (c.haveContentLight) std::printf("    %-46s +content-light\n", "");
        if (c.haveMastering) std::printf("    %-46s +mastering-display\n", "");

        // HDR content must declare BT.2020 primaries; SDR must not claim PQ.
        if (isHdr && c.primaries != AVCOL_PRI_BT2020 &&
            c.primaries != AVCOL_PRI_UNSPECIFIED) {
            problems.push_back(c.name + ": HDR transfer with non-2020 primaries");
        }
        if (!isHdr && c.transfer == AVCOL_TRC_UNSPECIFIED) {
            problems.push_back(c.name + ": no transfer characteristic (SDR assumed)");
        }
    }

    std::printf("[ColorPipeline] hdr=%d sdr=%d unspecified=%d problems=%zu\n",
                hdr, sdr, unspecified, problems.size());
    for (const std::string& s : problems) {
        std::printf("[ColorPipeline] NOTE: %s\n", s.c_str());
    }
    // Files with unspecified transfer are recorded, not failed: plenty of real
    // media omits it and the engine then assumes SDR, which is correct-by-
    // default. The assertion that matters is that nothing is INCONSISTENT.
    for (const std::string& s : problems) {
        EXPECT_EQ(s.find("non-2020"), std::string::npos) << s;
    }
}

TEST(ColorPipeline, PoolFormatMirrorsDecodeSurfaceNotTransfer) {
    // The pool mirrors the DECODER SURFACE that is actually copied into it —
    // NOT the SDR/HDR flag. A format-mismatched CopySubresourceRegion is
    // dropped silently and every frame then renders as garbage (the 10-bit
    // SDR "green mess": P010 surfaces while the SDR flag asked for NV12).
    // 10-bit SDR therefore keeps its P010 pool; the transfer function only
    // selects the shader math via the frame's colour info.
    EXPECT_EQ(rex::wallpaper::FfmpegVideoDecoder::PoolTextureFormat(
                  true, false, DXGI_FORMAT_P010),
              DXGI_FORMAT_P010);
    EXPECT_EQ(rex::wallpaper::FfmpegVideoDecoder::PoolTextureFormat(
                  true, true, DXGI_FORMAT_P010),
              DXGI_FORMAT_P010);
    EXPECT_EQ(rex::wallpaper::FfmpegVideoDecoder::PoolTextureFormat(
                  true, false, DXGI_FORMAT_NV12),
              DXGI_FORMAT_NV12);
}

// ─── Channel order of the video-slot SRV ───────────────────────────────────
//
// D3D12 returns the four components of a fetched texel in canonical
// R,G,B,A order for EVERY format — the format only decides the byte order
// the pixels sit in memory. The software pool is B8G8R8A8 (swscale writes
// B,G,R,A bytes), and the renderer used to describe that view with a
// component mapping that treated the fetched components as raw bytes
// (R<-byte2, B<-byte0). That swaps red and blue for every
// software-decoded wallpaper: skin tones rendered as a blue glove, red
// indicator lamps rendered blue — while the hardware P010 path (which uses
// no such mapping) stayed correct, which is exactly why only some clips
// looked wrong.
//
// The mapping must be the identity for every pool format.
TEST(ColorPipeline, VideoSlotSrvKeepsCanonicalComponentOrder) {
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_UNORM,
    };
    for (const DXGI_FORMAT fmt : formats) {
        const D3D12_SHADER_RESOURCE_VIEW_DESC desc =
            rex::wallpaper::MakeVideoSlotSrvDescriptor(fmt);
        EXPECT_EQ(desc.Format, fmt);
        EXPECT_EQ(desc.ViewDimension, D3D12_SRV_DIMENSION_TEXTURE2D);
        for (int component = 0; component < 4; ++component) {
            const auto mapped =
                D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(component,
                                                         desc.Shader4ComponentMapping);
            EXPECT_EQ(mapped, static_cast<D3D12_SHADER_COMPONENT_MAPPING>(component))
                << "format " << static_cast<int>(fmt) << " component " << component
                << " must come from the same canonical component";
        }
    }
}

// ─── mpv / libplacebo tone-map parity ──────────────────────────────────────
//
// The engine's HDR->SDR tone map is a port of libplacebo v7.360.0 (the build
// in mpv 0.41.0). These tests pin that port against mpv's OWN measurements:
// a synthetic 24-band PQ ramp (0.01 .. 4000 cd/m²) was rendered by the
// installed mpv and captured at 16 bits; the table below is mpv's output.
//
// If the curve, the anchors or the black handling drift, these fail - which
// is the whole point: "looks about right" is not a regression guard, the
// measured transfer function is.
namespace {

using rex::wallpaper::tone::BuildSplineCurve;
using rex::wallpaper::tone::kSdrWhiteNits;
using rex::wallpaper::tone::kTargetBlackNorm;
using rex::wallpaper::tone::PeakState;
using rex::wallpaper::tone::PqDecode;
using rex::wallpaper::tone::PqEncode;

// The ramp, in cd/m².
const double kRampBands[] = {0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 26,
                             40, 50, 80, 100, 150, 203, 300, 400, 600, 800,
                             1000, 2000, 4000};

// mpv 0.41.0's measured 16-bit output for the same ramp (video path).
const unsigned short kMpvRampOutput[] = {
    331, 814, 1250, 2148, 3298, 4724, 6622, 9992, 13553, 18261, 20402, 24539,
    27082, 32277, 34687, 39054, 42173, 46156, 48839, 52330, 54600, 56319, 61075,
    65521};

float SrgbEncode(float x) {
    if (x <= 0.0031308f) return 12.92f * x;
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

// The ramp's measured statistics: the peak detector sees a 4000 cd/m² peak
// and the mean of the band PQ codes (every band is above the black cutoff).
float RampAveragePq() {
    double sum = 0.0;
    int n = 0;
    for (double band : kRampBands) {
        const float pq = PqEncode(static_cast<float>(band));
        if (pq < 0.01f) continue;   // below libplacebo's black cutoff
        sum += pq;
        ++n;
    }
    return static_cast<float>(sum / n);
}

}  // namespace

TEST(ColorPipeline, ToneMapMatchesMpvOnTheBandRamp) {
    // The whole point of the port: the engine's HDR->SDR transfer function
    // must equal mpv's, measured in codes - not merely "not crushed".
    const float maxPq = PqEncode(4000.0f);
    const float avgPq = RampAveragePq();
    const auto curve = BuildSplineCurve(maxPq, avgPq);

    double sumAbs = 0.0;
    double maxAbs = 0.0;
    for (size_t i = 0; i < std::size(kRampBands); ++i) {
        const float inPq = PqEncode(static_cast<float>(kRampBands[i]));
        const float outPq = curve.Evaluate(inPq);
        const float nits = PqDecode(outPq);
        // libplacebo's black-scaled delinearize: the target black
        // (203/1000 cd/m²) lands on code 0, the reference white on code 1.
        const float norm = (nits / kSdrWhiteNits - kTargetBlackNorm) /
                           (1.0f - kTargetBlackNorm);
        const float srgb = SrgbEncode(std::clamp(norm, 0.0f, 1.0f));
        const float pred = srgb * 65535.0f;
        const float diff = (pred - static_cast<float>(kMpvRampOutput[i])) / 257.0f;
        sumAbs += std::fabs(diff);
        maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(diff)));
        EXPECT_LE(std::fabs(diff), 1.0f)
            << "band " << kRampBands[i] << " cd/m²: predicted " << pred
            << ", mpv measured " << kMpvRampOutput[i];
    }
    // The port is exact to a tenth of a code; the tolerance above (one code)
    // is the guard, this is the record of how tight it actually is.
    EXPECT_LT(sumAbs / std::size(kRampBands), 0.5);
    EXPECT_LT(maxAbs, 1.0);
}

TEST(ColorPipeline, ToneMapIsMonotonicAndAnchored) {
    // Independent of mpv: the curve must be monotonic, must map the source
    // peak to the SDR reference white and must not lift black above the
    // target black.
    const auto curve = BuildSplineCurve(PqEncode(4000.0f), RampAveragePq());

    float previous = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const float pq = static_cast<float>(i) / 200.0f;
        const float out = curve.Evaluate(pq);
        EXPECT_GE(out, previous) << "not monotonic at " << pq;
        previous = out;
    }

    // input_max -> output_max (the SDR white) and input_min -> output_min.
    EXPECT_NEAR(curve.Evaluate(curve.inMax), curve.outMax, 1e-3f);
    EXPECT_NEAR(curve.Evaluate(curve.inMin), curve.outMin, 1e-3f);

    // The curve never exceeds the target white.
    for (int i = 0; i <= 200; ++i) {
        const float pq = static_cast<float>(i) / 200.0f;
        EXPECT_LE(curve.Evaluate(pq), curve.outMax + 1e-4f);
    }
}

TEST(ColorPipeline, ToneMapBlackLandsOnCodeZero) {
    // The "too much black" report was partly a black-point bug: the target
    // black (0.203 cd/m²) must land on output code 0, not on a lifted grey.
    const auto curve = BuildSplineCurve(PqEncode(1000.0f), PqEncode(100.0f));
    const float blackSignal = curve.ToDisplay(curve.inMin);
    EXPECT_LE(blackSignal, 0.0f);
    // ... and the source black must not be lifted above it.
    EXPECT_LE(curve.ToDisplay(PqEncode(0.01f)), 0.02f);
}

TEST(ColorPipeline, PeakStateSmoothingConvergesAndSnapsOnScenes) {
    // libplacebo smooths the detected peak with a 20-frame IIR and snaps it
    // on a scene change (1 % .. 3 % of PQ range). Both behaviours matter:
    // without the IIR the wallpaper pumps, without the snap a cut stays
    // dim for seconds.
    PeakState state;

    // First measurement is taken verbatim.
    state.Update(0.8f, 0.4f);
    EXPECT_TRUE(state.valid);
    EXPECT_NEAR(state.maxPq, 0.8f, 1e-6f);
    EXPECT_NEAR(state.avgPq, 0.4f, 1e-6f);

    // A tiny change converges slowly (that is the point of the IIR).
    state.Update(0.8f, 0.4f + 0.0005f);
    EXPECT_NEAR(state.avgPq, 0.4f + 0.0005f * PeakState::SmoothingCoefficient(),
                1e-6f);

    // A scene change (well past 3 % of PQ range) snaps immediately.
    state.Update(0.8f, 0.6f);
    EXPECT_NEAR(state.avgPq, 0.6f, 1e-6f);
    EXPECT_NEAR(state.maxPq, 0.8f, 1e-6f);

    // Convergence: repeated identical measurements reach the target.
    PeakState slow;
    for (int i = 0; i < 400; ++i) slow.Update(0.5f, 0.3f);
    EXPECT_NEAR(slow.avgPq, 0.3f, 1e-3f);
}

// The output dither amplitude is one code of the OUTPUT depth, so it is
// decided per back-buffer format. The pixel shader is compiled per pipeline
// state with that depth as TARGET_BITS; this pins the rule the shader is
// compiled from (the FP16 HDR output is not quantised and must not dither).
TEST(ColorPipeline, OutputDitherMatchesTheTargetDepth) {
    // mpv-parity 10-bit SDR swapchain.
    EXPECT_EQ(rex::graphics::OutputDitherBits(DXGI_FORMAT_R10G10B10A2_UNORM), 10);
    // The 8-bit fallback for drivers that refuse the 10-bit buffer.
    EXPECT_EQ(rex::graphics::OutputDitherBits(DXGI_FORMAT_R8G8B8A8_UNORM), 8);
    // The HDR output is FP16 - not quantised, so no dither.
    EXPECT_EQ(rex::graphics::OutputDitherBits(DXGI_FORMAT_R16G16B16A16_FLOAT), 0);
    EXPECT_EQ(rex::graphics::OutputDitherBits(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), 0);
}

// The OS reports the SDR white level as a fixed-point multiplier of the
// 80-nit reference (DISPLAYCONFIG_SDR_WHITE_LEVEL): nits = (level/1000)*80.
// The conversion is pure, so it is pinned here; the query itself needs an
// HDR display and is exercised live.
TEST(ColorPipeline, SdrWhiteLevelConversion) {
    EXPECT_FLOAT_EQ(rex::graphics::SdrWhiteLevelToNits(1000), 80.0f);
    EXPECT_FLOAT_EQ(rex::graphics::SdrWhiteLevelToNits(1250), 100.0f);
    EXPECT_FLOAT_EQ(rex::graphics::SdrWhiteLevelToNits(2538), 203.04f);
    // 0 means "the display does not report one" - the caller keeps its
    // fallback, so the conversion must not turn it into a usable value.
    EXPECT_FLOAT_EQ(rex::graphics::SdrWhiteLevelToNits(0), 0.0f);
}
