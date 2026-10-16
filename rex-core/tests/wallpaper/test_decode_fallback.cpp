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

using rex::wallpaper::CheckHwProfileSupport;
using rex::wallpaper::DecodePathUnusable;
using rex::wallpaper::HwProfileSupport;

namespace {

// Step 2: hand-rolled GUIDs with values quoted against FFmpeg
// libavcodec/dxva2.c + mingw dxva2api.idl (Step 3 replaces these with
// string-parsed spec constants so the tests are independent of the table).
GUID MakeGuid(unsigned long d1, unsigned short d2, unsigned short d3,
              unsigned char b0, unsigned char b1, unsigned char b2,
              unsigned char b3, unsigned char b4, unsigned char b5,
              unsigned char b6, unsigned char b7) {
  GUID g;
  g.Data1 = d1;
  g.Data2 = d2;
  g.Data3 = d3;
  g.Data4[0] = b0;
  g.Data4[1] = b1;
  g.Data4[2] = b2;
  g.Data4[3] = b3;
  g.Data4[4] = b4;
  g.Data4[5] = b5;
  g.Data4[6] = b6;
  g.Data4[7] = b7;
  return g;
}

// DXVA2_ModeH264_VLD_NoFGT.
const GUID kH264Vld = MakeGuid(0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00,
                               0xc0, 0x4f, 0x2e, 0x73, 0xc5);
// DXVA2_ModeHEVC_VLD_Main10.
const GUID kHevcMain10 = MakeGuid(0x107af0e0, 0xef1a, 0x4d19, 0xab, 0xa8, 0x67,
                                  0xa1, 0x63, 0x07, 0x3d, 0x13);

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
