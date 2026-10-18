// DecodePathUnusable predicate tests (P2-1 latch, re-landed stepwise).
//
// Zero frames decoded after `budget` DecodeFrame attempts (or a send-fail
// streak on a never-decoded path) means the path is unusable, not slow:
// surface it so the supervisor can poison-park the file instead of
// stalling on it forever. Attempts reset on the first decoded frame, so a
// slow-but-working path never trips this.
//
// NOTE: the per-codec GUID probe + its tests re-land separately (Step 2)
// with string-parsed constants; this file covers only the latch predicate.

#include <gtest/gtest.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <gtest/gtest.h>

#include <cstdio>

#include <d3d11_4.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using rex::wallpaper::CheckHwProfileSupport;
using rex::wallpaper::D3d11vaCanDecodeChroma;
using rex::wallpaper::DecodePathUnusable;
using rex::wallpaper::HwProfileSupport;
using rex::wallpaper::SwsCacheHit;
using rex::wallpaper::SwsCacheKey;
using rex::wallpaper::SwsColorParams;
using rex::wallpaper::SwsColorParamsFor;
using rex::wallpaper::VideoColorInfo;

namespace {

// Parses a canonical "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" GUID string.
// The strings below are quoted verbatim against FFmpeg libavcodec/dxva2.c
// (ff_DXVA2_Mode*), mingw-w64 dxva2api.idl, Microsoft Learn "Direct3D 12
// video GUIDs", and Firefox DXVA2Manager.cpp — this is what makes the tests
// independent of the table under test. 2026-10-16: the table shipped with
// fabricated Data4 tails built by hand-rolled byte helpers like the old
// MakeGuid here; parsing spec strings is the structural fix.
GUID GuidFromString(const char* s) {
  GUID g{};
  unsigned int d4[8];
  const int n =
      std::sscanf(s, "%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  &g.Data1, &g.Data2, &g.Data3, &d4[0], &d4[1], &d4[2],
                  &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]);
  EXPECT_EQ(n, 11) << "bad GUID literal in test: " << s;
  for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(d4[i]);
  return g;
}

// DXVA2_ModeH264_VLD_NoFGT (empirically proven too: H.264 opens on hardware).
const GUID kH264Vld =
    GuidFromString("1b81be68-a0c7-11d3-b984-00c04f2e73c5");
// DXVA2_ModeHEVC_VLD_Main10.
const GUID kHevcMain10 =
    GuidFromString("107af0e0-ef1a-4d19-aba8-67a163073d13");

}  // namespace

TEST(HwProfileProbe, H264GuidMeansH264Supported) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, &kH264Vld, 1),
            HwProfileSupport::Supported);
}

TEST(HwProfileProbe, H264GuidMeansAv1Unsupported) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_AV1, &kH264Vld, 1),
            HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, EmptyListIsUnsupportedForKnownCodecs) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, nullptr, 0),
            HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, UnknownCodecAttemptsHardware) {
  const GUID list[] = {kH264Vld, kHevcMain10};
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_MPEG4, list, 2),
            HwProfileSupport::UnknownCodec);
}

TEST(HwProfileProbe, AnyFamilyGuidMatches) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_HEVC, &kHevcMain10, 1),
            HwProfileSupport::Supported);
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, &kHevcMain10, 1),
            HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, EveryTableEntryMatchesItsCanonicalString) {
  // Pins each shipped constant against its spec string: a single wrong
  // nibble anywhere in the table fails here instead of silently forcing
  // software decode with wrong colors (2026-10-16).
  struct Case {
    const char* guid;
    AVCodecID codec;
  };
  const Case cases[] = {
      {"1b81be68-a0c7-11d3-b984-00c04f2e73c5", AV_CODEC_ID_H264},
      {"5b11d51b-2f4c-4452-bcc3-09f2a1160cc0", AV_CODEC_ID_HEVC},
      {"107af0e0-ef1a-4d19-aba8-67a163073d13", AV_CODEC_ID_HEVC},
      {"463707f8-a1d0-4585-876d-83aa6d60b89e", AV_CODEC_ID_VP9},
      {"a4c749ef-6ecf-48aa-8448-50a7a1165ff7", AV_CODEC_ID_VP9},
      {"b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a", AV_CODEC_ID_AV1},
      {"ee27417f-5e77-4d14-a53b-7dc2ed571ff0", AV_CODEC_ID_MPEG2VIDEO},
      {"1b81be94-a0c7-11d3-b984-00c04f2e73c5", AV_CODEC_ID_VC1},
  };
  for (const auto& c : cases) {
    const GUID g = GuidFromString(c.guid);
    EXPECT_EQ(CheckHwProfileSupport(c.codec, &g, 1),
              HwProfileSupport::Supported)
        << "table mismatch for " << c.guid;
  }
  const GUID hevc = GuidFromString("5b11d51b-2f4c-4452-bcc3-09f2a1160cc0");
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_AV1, &hevc, 1),
            HwProfileSupport::Unsupported);
}

TEST(DecodePathUnusable, ZeroFramesPastBudgetIsUnusable) {
  EXPECT_TRUE(DecodePathUnusable(0, 240, 240));
  EXPECT_TRUE(DecodePathUnusable(0, 1000, 240));
}

TEST(DecodePathUnusable, SlowButWorkingNeverTrips) {
  EXPECT_FALSE(DecodePathUnusable(0, 239, 240));
  EXPECT_FALSE(DecodePathUnusable(1, 10000, 240));
  EXPECT_FALSE(DecodePathUnusable(5, 0, 240));
}

TEST(DecodePathUnusable, CustomBudget) {
  EXPECT_TRUE(DecodePathUnusable(0, 8, 8));
  EXPECT_FALSE(DecodePathUnusable(0, 7, 8));
}

// ─── D3D11VA chroma gate (2026-10-17) ───────────────────────────────────
//
// The Oppenheimer Rext 4:4:4 clip opens with hwaccel attached, then FFmpeg
// falls back to software decode INSIDE the stream while keeping the
// hardware path's single-thread pin — measured 92-121 ms/frame (~8-11 fps)
// vs 21 ms/frame with 8 threads. D3D11VA only ever produces 4:2:0
// surfaces; skipping the doomed attempt up front keeps software decode on
// auto threads.

TEST(HwChromaGate, FourTwentyFormatsAttemptHardware) {
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV420P));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV420P10LE));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV420P12LE));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_NV12));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_P010LE));
}

TEST(HwChromaGate, NonFourTwentyRefused) {
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV444P10LE));  // Rext clip
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV444P));
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV422P));
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_GRAY8));
}

TEST(HwChromaGate, UnknownFormatKeepsStatusQuo) {
  // AV_PIX_FMT_NONE means "not resolved yet": attempt hardware, never
  // refuse on uncertainty (same rule as HwProfileSupport::UnknownCodec).
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_NONE));
}

// ─── swscale colorimetry policy (2026-10-17) ────────────────────────────
//
// Every SwsContext creation must apply the source matrix/range; contexts
// left on swscale defaults convert BT.709 sources with the BT.601 matrix
// (deadpool tint, 2026-10-16). The cache key carries matrix and range so a
// context created before colorimetry was known can never be reused once it
// is known.

TEST(SwsColorPolicy, KnownMatricesMapToSwsCoefficients) {
  const auto p709 = SwsColorParamsFor(VideoColorInfo::Matrix::BT709, false);
  EXPECT_EQ(p709.srcCsp, SWS_CS_ITU709);
  EXPECT_EQ(p709.srcRange, 0);
  EXPECT_EQ(p709.dstCsp, SWS_CS_ITU709);
  EXPECT_EQ(p709.dstRange, 1);

  const auto p601 = SwsColorParamsFor(VideoColorInfo::Matrix::BT601, false);
  EXPECT_EQ(p601.srcCsp, SWS_CS_SMPTE170M);
  EXPECT_EQ(p601.dstCsp, SWS_CS_SMPTE170M);

  const auto p2020 = SwsColorParamsFor(VideoColorInfo::Matrix::BT2020, true);
  EXPECT_EQ(p2020.srcCsp, SWS_CS_BT2020);
  EXPECT_EQ(p2020.srcRange, 1);  // full-range source flips src range only
  EXPECT_EQ(p2020.dstRange, 1);  // destination is always full-range RGB
}

TEST(SwsColorPolicy, FullRangeSourceFlipsSrcRangeOnly) {
  const auto p = SwsColorParamsFor(VideoColorInfo::Matrix::BT709, true);
  EXPECT_EQ(p.srcRange, 1);
  EXPECT_EQ(p.dstCsp, SWS_CS_ITU709);
  EXPECT_EQ(p.dstRange, 1);
}

TEST(SwsColorPolicy, CacheHitRequiresIdenticalColorimetry) {
  const SwsCacheKey want{AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 0};
  EXPECT_TRUE(SwsCacheHit(want, want));

  // A context created with different (or default 601) colorimetry must
  // never be reused for a 709 source.
  EXPECT_FALSE(
      SwsCacheHit({AV_PIX_FMT_YUV444P10LE, false, SWS_CS_SMPTE170M, 0}, want));
  EXPECT_FALSE(
      SwsCacheHit({AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 1}, want));
  EXPECT_FALSE(
      SwsCacheHit({AV_PIX_FMT_YUV444P10LE, true, SWS_CS_ITU709, 0}, want));
  EXPECT_FALSE(
      SwsCacheHit({AV_PIX_FMT_YUV420P10LE, false, SWS_CS_ITU709, 0}, want));
}

TEST(SwsColorPolicy, DefaultKeyNeverHits) {
  // Default-constructed key == the state before any context was created.
  const SwsCacheKey want{AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 0};
  EXPECT_FALSE(SwsCacheHit(SwsCacheKey{}, want));
}
