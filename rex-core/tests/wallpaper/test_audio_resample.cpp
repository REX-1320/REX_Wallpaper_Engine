// ═══════════════════════════════════════════════════════════════════════════
// P1-13 regression tests: audio tempo resample (R20)
// ═══════════════════════════════════════════════════════════════════════════
//
// SetSpeed paced only the video loop — audio played at 1x, desyncing by
// construction at any other speed. The render step now consumes
// speed x device frames of content per callback, resampled by linear
// interpolation (per channel, S16 in/out, no overflow).

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "wallpaper/audio/AudioPlayer.hpp"

using namespace rex::wallpaper;

TEST(AudioResampleTest, IdentityPassesThrough) {
    const std::vector<int16_t> src = {1000, -2000, 3000, -4000};
    std::vector<int16_t> dst(4, 0);
    AudioPlayer::ResampleLinear(src.data(), 4, 1, dst.data(), 4);
    EXPECT_EQ(dst, src);
}

TEST(AudioResampleTest, DecimationPicksEvenly) {
    // 4 content frames -> 2 device frames: endpoints, linear in time.
    const std::vector<int16_t> src = {0, 1000, 2000, 3000};
    std::vector<int16_t> dst(2, 0);
    AudioPlayer::ResampleLinear(src.data(), 4, 1, dst.data(), 2);
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 3000);
}

TEST(AudioResampleTest, StretchInterpolatesMidpoints) {
    // 2 content frames -> 4 device frames: 0, 1/3, 2/3, full.
    const std::vector<int16_t> src = {0, 3000};
    std::vector<int16_t> dst(4, 0);
    AudioPlayer::ResampleLinear(src.data(), 2, 1, dst.data(), 4);
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 1000);
    EXPECT_EQ(dst[2], 2000);
    EXPECT_EQ(dst[3], 3000);
}

TEST(AudioResampleTest, ChannelsStayIndependent) {
    // Stereo: L ramps up, R ramps down — no cross-talk.
    const std::vector<int16_t> src = {0, 3000, 3000, 0};
    std::vector<int16_t> dst(4, 0);
    AudioPlayer::ResampleLinear(src.data(), 2, 2, dst.data(), 2);
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 3000);
    EXPECT_EQ(dst[2], 3000);
    EXPECT_EQ(dst[3], 0);
}

TEST(AudioResampleTest, SingleFrameEdges) {
    const std::vector<int16_t> src = {1234};
    std::vector<int16_t> dst(3, 0);
    AudioPlayer::ResampleLinear(src.data(), 1, 1, dst.data(), 3);
    EXPECT_EQ(dst[0], 1234);
    EXPECT_EQ(dst[1], 1234);
    EXPECT_EQ(dst[2], 1234);

    const std::vector<int16_t> src2 = {100, 200, 300};
    std::vector<int16_t> dst2(1, 0);
    AudioPlayer::ResampleLinear(src2.data(), 3, 1, dst2.data(), 1);
    EXPECT_EQ(dst2[0], 100);
}

TEST(AudioResampleTest, ExtremesDoNotOverflow) {
    const std::vector<int16_t> src = {32767, -32768, 32767, -32768};
    std::vector<int16_t> dst(8, 0);
    AudioPlayer::ResampleLinear(src.data(), 4, 1, dst.data(), 8);
    for (int16_t v : dst) {
        EXPECT_GE(v, -32768);
        EXPECT_LE(v, 32767);
    }
    EXPECT_EQ(dst[0], 32767);
    EXPECT_EQ(dst[7], -32768);
}
