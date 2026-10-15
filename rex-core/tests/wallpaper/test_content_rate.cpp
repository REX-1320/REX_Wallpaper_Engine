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
//   4. Loop wrap starts a new epoch (no negative rate); the first full
//      window after it folds to ~1.0 again.
//   5. Decode-order B-frame jitter (backward steps of a frame or two)
//      does not corrupt the reading — the watermark ignores reorder wobble.
//   6. Seeks (forward past kMaxJumpSec, backward past the reorder slack)
//      start a new epoch — a seek must never read as a rate excursion.
//   7. A freeze-recovery jump is clamped — it reads low, never a burst.
//   8. No full window yet -> last rate (0.0 initially), never a partial guess.
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

TEST(ContentRateMeter, LoopWrapStartsNewEpoch) {
    // A loop wrap (PTS falls back near zero) is a new epoch, not negative
    // content: the window rebases at the wrap, and the first full window
    // after it folds to ~1.0 again.
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);
    pts = 0.033;  // decoder looped: PTS restarts near zero
    m.OnPresented(pts, 47.55, wall);
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    const double rate = m.Fold(wall);
    ASSERT_GE(rate, 0.0);  // never negative across the wrap
    EXPECT_NEAR(rate, 1.0, 0.01);
}

TEST(ContentRateMeter, DecodeOrderBFrameJitterReadsOne) {
    // THE BUG THIS GUARDS (measured 2026-10-15): pushes arrive in DECODE
    // order, so B-frames step PTS backward by a frame or two constantly
    // (0, 0.066, 0.033, 0.100, 0.083, ...). A signed-delta meter rebases
    // its window on every jitter step and reads ~0.01 while content plays
    // perfectly at 1x. The watermark ignores reorder wobble.
    ContentRateMeter m;
    double wall = 0.0;
    // I(0.000) P(0.066) B(0.033) P(0.100) B(0.083) ... at 30 fps content,
    // presented as decoded, wall advancing 1/30 per frame.
    const double step = 1.0 / 30.0;
    double pts = 0.0;
    for (int i = 0; i < 30 * 12; ++i) {
        wall += 1.0 / 30.0;
        // B-frame pattern: every third frame arrives one step early.
        const int k = i % 3;
        pts = (i / 3) * 3 * step + (k == 1 ? 2 * step : (k == 2 ? step : 0.0));
        m.OnPresented(pts, 600.0, wall);
    }
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.05);
}

TEST(ContentRateMeter, FreezeRecoveryJumpStartsNewEpoch) {
    // After an 8 s freeze the first current frame jumps 8 s forward in one
    // present. An 8 s jump is indistinguishable from an explicit forward
    // seek, so it starts a new epoch: no spike, no negative - the first
    // full window after it folds clean.
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);
    wall += 8.0;   // 8 s of frozen wall time (no presents)
    pts += 8.0;    // ...then one current frame jumps the watermark forward
    m.OnPresented(pts, 600.0, wall);
    EXPECT_LT(m.Fold(wall), 1.5);  // new epoch: last rate, never a spike
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);  // clean window again
}

TEST(ContentRateMeter, IsolatedSubSecondJumpsAreClamped) {
    // Five isolated +0.8 s catch-up jumps inside one window: each counts
    // kMaxStepSec (0.5 s), not 0.8. Clamped total 11 + 2.5 = 13.5 over
    // ~11.2 s -> ~1.21; unclamped it would read (11 + 4) / 11.2 = 1.34.
    ContentRateMeter m;
    double wall = 0.0, pts = 0.0;
    RunSteady(m, 11.0, 30.0, 1.0, 600.0, wall, pts);
    EXPECT_NEAR(m.Fold(wall), 1.0, 0.01);
    for (int j = 0; j < 5; ++j) {
        RunSteady(m, 2.0, 30.0, 1.0, 600.0, wall, pts);
        pts += 0.8;  // isolated catch-up jump (below the seek threshold)
        wall += 1.0 / 30.0;
        m.OnPresented(pts, 600.0, wall);
    }
    RunSteady(m, 1.0, 30.0, 1.0, 600.0, wall, pts);
    const double rate = m.Fold(wall);
    EXPECT_GT(rate, 1.1);   // the jumps genuinely advanced content
    EXPECT_LT(rate, 1.25);  // ...but each counted 0.5, never 0.8
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
