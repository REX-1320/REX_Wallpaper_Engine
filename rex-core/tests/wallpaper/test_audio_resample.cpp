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
#include <cmath>
#include <cstdlib>
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

// The tempo path resamples with a windowed sinc instead of linear
// interpolation. Two properties matter:
//
//  1. pass-through: the same rate must come out unchanged;
//  2. a tone ABOVE the new Nyquist must not survive - linear interpolation
//     folds it back (aliasing, an audible whistle), the sinc filters it.

TEST(AudioResampleTest, WindowedSincPassesThrough) {
    // A smooth signal: an 8-tap windowed kernel cannot reconstruct content
    // that sits at Nyquist (no finite kernel can), so the pass-through
    // property is pinned on a signal the kernel can actually reproduce.
    std::vector<int16_t> src(64);
    for (size_t i = 0; i < src.size(); ++i) {
        const double t = static_cast<double>(i) / src.size();
        src[i] = static_cast<int16_t>(
            12000.0 * std::sin(2.0 * 3.14159265358979 * 0.05 * t));
    }
    std::vector<int16_t> dst(src.size(), 0);
    AudioPlayer::ResampleWindowedSinc(src.data(), src.size(), 1, dst.data(),
                                      dst.size());
    for (size_t i = 0; i < src.size(); ++i) {
        ASSERT_LE(std::abs(static_cast<int>(dst[i]) - static_cast<int>(src[i])), 24)
            << "sample " << i << ": " << dst[i] << " vs " << src[i];
    }
}

TEST(AudioResampleTest, WindowedSincFiltersWhatLinearWouldAlias) {
    // 0.45 of the sample rate: well above the 0.25 fs Nyquist of a 2:1
    // decimation.
    const size_t n = 64;
    std::vector<int16_t> src(n);
    for (size_t i = 0; i < n; ++i) {
        src[i] = static_cast<int16_t>(
            12000.0 * std::sin(2.0 * 3.14159265358979 * 0.45 * static_cast<double>(i)));
    }
    std::vector<int16_t> linear(n / 2, 0);
    std::vector<int16_t> sinc(n / 2, 0);
    AudioPlayer::ResampleLinear(src.data(), n, 1, linear.data(), linear.size());
    AudioPlayer::ResampleWindowedSinc(src.data(), n, 1, sinc.data(),
                                      sinc.size());

    auto rms = [](const std::vector<int16_t>& v) {
        double sum = 0.0;
        for (int16_t s : v) sum += static_cast<double>(s) * s;
        return std::sqrt(sum / static_cast<double>(v.size()));
    };
    const double linearRms = rms(linear);
    const double sincRms = rms(sinc);
    EXPECT_GT(linearRms, 500.0)
        << "the linear resample is expected to alias this tone back in";
    EXPECT_LT(sincRms, linearRms * 0.5)
        << "the windowed sinc must filter the aliased band out (linear "
        << linearRms << " vs sinc " << sincRms << ")";
}
