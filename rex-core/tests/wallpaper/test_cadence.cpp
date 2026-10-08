// ═══════════════════════════════════════════════════════════════════════════
// P2-15 regression tests: render cadence follows the slower of maxFps and
// measured decoder cadence, with a clamped maxFps (R18)
// ═══════════════════════════════════════════════════════════════════════════
//
// PaceLoop promised "the SLOWER of maxFps and the decoder's measured
// cadence" but implemented maxFps-only (full-rate wakeups against a 30 fps
// decode), and the IPC maxFps was unclamped (absurd values → 4 ms floor).

#include <gtest/gtest.h>

#include "wallpaper/engine/CadencePolicy.hpp"

using namespace rex::wallpaper;

TEST(CadencePolicyTest, DefaultMaxFpsIsSixty) {
    EXPECT_DOUBLE_EQ(RenderIntervalMs(0, 0.0), 1000.0 / 60.0);
    EXPECT_DOUBLE_EQ(RenderIntervalMs(-5, 0.0), 1000.0 / 60.0);
}

TEST(CadencePolicyTest, MaxFpsDrivesInterval) {
    EXPECT_DOUBLE_EQ(RenderIntervalMs(30, 0.0), 1000.0 / 30.0);
}

TEST(CadencePolicyTest, AbsurdMaxFpsIsClamped) {
    // 100000 fps would slam the 4 ms floor; clamp to 240 first.
    EXPECT_DOUBLE_EQ(RenderIntervalMs(100000, 0.0), 1000.0 / 240.0);
}

TEST(CadencePolicyTest, SlowerDecoderCadenceWins) {
    EXPECT_DOUBLE_EQ(RenderIntervalMs(60, 30.0), 1000.0 / 30.0);
}

TEST(CadencePolicyTest, FasterDecoderDoesNotRaiseRate) {
    EXPECT_DOUBLE_EQ(RenderIntervalMs(30, 120.0), 1000.0 / 30.0);
}

TEST(CadencePolicyTest, FloorBoundsTinyIntervals) {
    // Even absurd inputs never pace below 4 ms.
    EXPECT_GE(RenderIntervalMs(240, 1000.0), 4.0);
}
