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
using rex::wallpaper::DecodePathUnusable;
using rex::wallpaper::HwProfileSupport;

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
