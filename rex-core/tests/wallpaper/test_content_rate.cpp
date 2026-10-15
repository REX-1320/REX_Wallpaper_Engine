// ContentRateMeter tests (P0-2 content-rate check).
//
// THE BLIND SPOT THIS GUARDS: position reported 1.000x and effFps reported
// the source rate while displayed CONTENT ran at ~2x (2026-10-15 scheduler
// defect). The meter divides presented-PTS advance by wall time, so it is
// the one metric that derives from the content itself.
//
// Contract under test:
//   1. Realtime playback folds to ~1.0.
//   2. Content advancing twice as fast as the wall folds to ~2.0 (the defect).
//   3. Frozen content (no presents) folds to 0 — a freeze IS a rate of 0.
//   4. Loop wrap (PTS backward jump through duration) does not corrupt the
//      rate — it unrolls as one more frame of content.
//   5. Seeks (>= kMaxJumpSec, either direction) are discarded and rebase the
//      window — a seek must never read as a rate excursion.
//   6. No full window yet -> last rate (0.0 initially), never a partial guess.
//
// NOTE: every steady run is 11 s, one second past the 10 s window, because
// Fold() intentionally refuses to return a partial-window rate.

#include <gtest/gtest.h>

#include <cmath>

#include "wallpaper/playback/ContentRateMeter.hpp"

using rex::wallpaper::ContentRateMeter;

namespace {

// Advance a meter through `seconds` of wall clock at `wallFps` presents per
// second; content advances `speed`x realtime (pts step = speed / wallFps).
// `wall` and `pts` are caller-owned so continuations across seeks stay
// explicit.
void RunSteady(ContentRateMeter& m, double seconds, double wallFps,
               double speed, double duration, double& wall, double& pts) {
    const int n = static_cast<int>(seconds * wallFps);
    for (int i = 0; i < n; ++i) {
        wall += 1.0 / wallFps;
        pts += speed / wallFps;
        m.OnPresented(std::fmod(pts, duration), duration, wall);
    }
}

}  // namespace

TEST(ContentRateMeter, StartsAtZeroUntilFirstWindow) {
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    // 5 s of perfect playback is still inside the 10 s window.
    RunSteady(m, 5.0, 30.0, 1.0, 60.0, wall, pts);
    EXPECT_DOUBLE_EQ(m.Fold(wall + 1.0), 0.0);
    EXPECT_DOUBLE_EQ(m.LastRate(), 0.0);
}

TEST(ContentRateMeter, RealtimeFoldsToOne) {
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 60.0, wall, pts);
    const double rate = m.Fold(wall);
    ASSERT_GT(rate, 0.0);
    EXPECT_NEAR(rate, 1.0, 0.01);
}

TEST(ContentRateMeter, DoubleSpeedContentFoldsToTwo) {
    // The defect class this metric exists to catch: content advances twice
    // as fast as the wall clock while everything else reports healthy.
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 2.0, 60.0, wall, pts);
    const double rate = m.Fold(wall);
    ASSERT_GT(rate, 0.0);
    EXPECT_NEAR(rate, 2.0, 0.05);
}

TEST(ContentRateMeter, HalfSpeedContentFoldsToOneHalf) {
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 0.5, 60.0, wall, pts);
    const double rate = m.Fold(wall);
    ASSERT_GT(rate, 0.0);
    EXPECT_NEAR(rate, 0.5, 0.02);
}

TEST(ContentRateMeter, FreezeFoldsToZero) {
    // No presents at all after a clean window: content is frozen, which IS
    // a rate of 0 (this doubles as an underrun/freeze detector).
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 60.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);
    const double rate = m.Fold(wall + 15.0);  // 15 s with zero presents
    EXPECT_NEAR(rate, 0.0, 1e-9);
}

TEST(ContentRateMeter, LoopWrapDoesNotCorruptRate) {
    // Loop wrapping: pts runs up to duration then wraps to ~0. Counted raw
    // the window would collapse; unrolled it is just one more frame.
    ContentRateMeter m;
    const double duration = 3.0;
    const double wallFps = 30.0;
    double wall = 0.0, pts = 0.0;
    for (int i = 0; i < 30 * 12; ++i) {  // 4 loops of a 3 s clip
        wall += 1.0 / wallFps;
        pts += 1.0 / wallFps;
        m.OnPresented(std::fmod(pts, duration), duration, wall);
    }
    const double rate = m.Fold(wall);
    ASSERT_GT(rate, 0.0);
    EXPECT_NEAR(rate, 1.0, 0.02);
}

TEST(ContentRateMeter, ForwardSeekIsDiscardedAndRebases) {
    // A +100 s jump at one wall instant must not read as a fast-forward
    // burst: the seek sample is dropped and the window restarts there.
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);
    pts += 100.0;
    m.OnPresented(pts, 600.0, wall);  // explicit seek: discarded + rebase
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);  // clean window, no spike
}

TEST(ContentRateMeter, BackwardSeekIsDiscardedAndRebases) {
    // A -300 s jump must not collapse the rate negative or inflate it via
    // duration-unroll: treated as a seek, window restarted.
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 24.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);
    pts -= 300.0;
    m.OnPresented(pts, 600.0, wall);  // explicit seek: discarded + rebase
    RunSteady(m, 11.0, 24.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);  // never negative, never spiked
}

TEST(ContentRateMeter, ResetStartsFresh) {
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 60.0, wall, pts);
    EXPECT_GT(m.Fold(wall), 0.0);
    m.Reset();
    EXPECT_DOUBLE_EQ(m.LastRate(), 0.0);
    EXPECT_DOUBLE_EQ(m.Fold(wall + 1.0), 0.0);  // unarmed again
}
