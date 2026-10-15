// ResolveFrameRate tests (P0-1 source-rate root cause).
//
// THE DEFECT THIS GUARDS: marvel-spiderman-miles-morales.1920x1080.mp4
// declares r_frame_rate=60/1 but carries 1222 video samples over 47.554 s =
// 25.69 fps (proven by parsing the mp4 stsz/mvhd atoms). Trusting the
// declared 60 fps paces decode 2.3x too fast, so the ring fills with future
// frames that no scheduler can play correctly: permanent starve under the
// fixed scheduler's retain policy (effFps=0 for 37 s), ~2.4x content under
// the newest-published fallback. The user-visible "2x" was this file
// playing at the wrong rate, not (only) a scheduler bug.
//
// Contract under test:
//   1. Measured count/duration wins when the container provides both.
//   2. Agreement with the declared rate changes nothing.
//   3. Missing count / missing declared rate fall back gracefully.
//   4. Garbage counts and tiny durations never win.

#include <gtest/gtest.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using rex::wallpaper::ResolveFrameRate;

TEST(ResolveFrameRate, MarvelFileMeasuresTwentyFivePointSixNine) {
    // The file that motivated this: 1222 samples / 47.554 s, declared 60.
    bool usedCount = false;
    const double rate = ResolveFrameRate(1222, 47.554, 60.0, &usedCount);
    EXPECT_TRUE(usedCount);
    EXPECT_NEAR(rate, 25.69, 0.01);
}

TEST(ResolveFrameRate, AgreementKeepsDeclared) {
    // 1440 frames / 60 s = 24.0 == declared 24.0: identical either way.
    bool usedCount = false;
    const double rate = ResolveFrameRate(1440, 60.0, 24.0, &usedCount);
    EXPECT_TRUE(usedCount);
    EXPECT_NEAR(rate, 24.0, 1e-9);
}

TEST(ResolveFrameRate, MissingCountFallsBackToDeclared) {
    // Live/unknown-length streams report no count: trust the declaration.
    bool usedCount = true;
    EXPECT_DOUBLE_EQ(ResolveFrameRate(0, 60.0, 30.0, &usedCount), 30.0);
    EXPECT_FALSE(usedCount);
}

TEST(ResolveFrameRate, MissingDeclaredFallsBackToThirty) {
    bool usedCount = true;
    EXPECT_DOUBLE_EQ(ResolveFrameRate(0, 0.0, 0.0, &usedCount), 30.0);
    EXPECT_FALSE(usedCount);
}

TEST(ResolveFrameRate, MissingDeclaredUsesCount) {
    bool usedCount = false;
    EXPECT_NEAR(ResolveFrameRate(720, 30.0, 0.0, &usedCount), 24.0, 1e-9);
    EXPECT_TRUE(usedCount);
}

TEST(ResolveFrameRate, AbsurdCountNeverWins) {
    // Estimated/garbage counts (billions of frames, sub-1 fps, >480 fps,
    // sub-second durations) must not pace decode.
    bool usedCount = true;
    EXPECT_DOUBLE_EQ(ResolveFrameRate(1000000000LL, 47.5, 30.0, &usedCount),
                     30.0);
    EXPECT_FALSE(usedCount);
    EXPECT_DOUBLE_EQ(ResolveFrameRate(10, 47.5, 30.0, &usedCount), 30.0);
    EXPECT_FALSE(usedCount);
    EXPECT_DOUBLE_EQ(ResolveFrameRate(100000, 47.5, 30.0, &usedCount), 30.0);
    EXPECT_FALSE(usedCount);
    EXPECT_DOUBLE_EQ(ResolveFrameRate(100, 0.5, 30.0, &usedCount), 30.0);
    EXPECT_FALSE(usedCount);
}

TEST(ResolveFrameRate, NullFlagIsSafe) {
    // usedCount is optional.
    EXPECT_NEAR(ResolveFrameRate(1222, 47.554, 60.0, nullptr), 25.69, 0.01);
}
