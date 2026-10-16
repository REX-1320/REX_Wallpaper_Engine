// Decode-fallback policy tests (P2-1 capability-aware fallback).
//
// Contract under test:
//   1. A driver profile list containing the codec's GUID reports Supported.
//   2. A non-empty list without the codec's GUID reports Unsupported — the
//      caller skips the doomed hardware open and goes straight to software.
//   3. A codec REX carries no GUIDs for reports UnknownCodec — the caller
//      attempts hardware exactly as before (never refuse on uncertainty).
//   4. The unusable-path predicate fires only on zero-frames-past-budget,
//      never on a slow-but-working path.

#include <gtest/gtest.h>

#include <d3d11_4.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using rex::wallpaper::CheckHwProfileSupport;
using rex::wallpaper::DecodePathUnusable;
using rex::wallpaper::HwProfileSupport;

namespace {

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
const GUID kHevcMain10 = MakeGuid(0x107af0e0, 0xef1a, 0x4d19, 0xab, 0xb8, 0x67,
                                  0xa1, 0x63, 0x49, 0x7c, 0x4c);

}  // namespace

TEST(HwProfileProbe, H264GuidMeansH264Supported) {
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, &kH264Vld, 1),
              HwProfileSupport::Supported);
}

TEST(HwProfileProbe, H264GuidMeansAv1Unsupported) {
    // The RX 6700 XT case: H.264 decodes in hardware, AV1 must go straight
    // to software without a doomed D3D11VA attempt.
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_AV1, &kH264Vld, 1),
              HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, EmptyListIsUnsupportedForKnownCodecs) {
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, nullptr, 0),
              HwProfileSupport::Unsupported);
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_HEVC, nullptr, 0),
              HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, UnknownCodecAttemptsHardware) {
    // REX carries no MPEG-4 Part 2 GUIDs: an MSDN-quirk GUID list must not
    // refuse hardware for a codec the table doesn't cover.
    const GUID list[] = {kH264Vld, kHevcMain10};
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_MPEG4, list, 2),
              HwProfileSupport::UnknownCodec);
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_NONE, list, 2),
              HwProfileSupport::UnknownCodec);
}

TEST(HwProfileProbe, AnyFamilyGuidMatches) {
    // Main10-only driver still decodes HEVC (the profile table lists both
    // Main and Main10 GUIDs for the codec).
    EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_HEVC, &kHevcMain10, 1),
              HwProfileSupport::Supported);
    // ...but not H.264.
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
