// ═══════════════════════════════════════════════════════════════════════════
// CadenceController table tests (Phase 3)
// ═══════════════════════════════════════════════════════════════════════════
//
// presentationTarget = min(sourceFps × rate, displayHz, userCap) is the
// MAXIMUM USEFUL presentation cadence — never a decoder throttle.

#include <gtest/gtest.h>

#include <limits>

#include "wallpaper/playback/CadenceController.hpp"

using namespace rex::wallpaper;

TEST(CadenceControllerTest, SixtySixtyStaysSixty) {
    EXPECT_DOUBLE_EQ(CadenceController::PresentationTarget(60.0, 1.0f, 60, 60), 60.0);
}

TEST(CadenceControllerTest, HighFpsClampsToDisplay) {
    EXPECT_DOUBLE_EQ(CadenceController::PresentationTarget(120.0, 1.0f, 60, 0), 60.0);
}

TEST(CadenceControllerTest, HighHzClampsToUserCap) {
    EXPECT_DOUBLE_EQ(CadenceController::PresentationTarget(60.0, 1.0f, 160, 60), 60.0);
}

TEST(CadenceControllerTest, HighFpsHighHzPassesSource) {
    EXPECT_DOUBLE_EQ(CadenceController::PresentationTarget(120.0, 1.0f, 144, 0), 120.0);
}

TEST(CadenceControllerTest, LowFpsLimitsCadence) {
    EXPECT_DOUBLE_EQ(CadenceController::PresentationTarget(30.0, 1.0f, 160, 0), 30.0);
}

TEST(CadenceControllerTest, RateScalesDemand) {
    EXPECT_DOUBLE_EQ(CadenceController::PresentationTarget(60.0, 2.0f, 144, 0), 120.0);
}

TEST(CadenceControllerTest, FractionalSourcePreserved) {
    EXPECT_NEAR(CadenceController::PresentationTarget(59.94, 1.0f, 60, 60),
                59.94, 1e-9);
}

TEST(CadenceControllerTest, DegenerateInputsFallBack) {
    EXPECT_GT(CadenceController::PresentationTarget(0.0, 1.0f, 60, 60), 0.0);
    EXPECT_GT(CadenceController::PresentationTarget(60.0, 1.0f, 0, 60), 0.0);
}

// ─── Decode-ahead reserve policy (Phase D) ─────────────────────────────────
//
// The reserve is hard-capped by the shared 8-slot pool, so these tests pin the
// MEASURED depth arithmetic rather than the nominal 200 ms target.

TEST(DecodeAheadPolicy, ReserveMsIsFrameCountTimesSourceFrameTime) {
    EXPECT_NEAR(CadenceController::ReserveMs(5, 24.0), 208.333, 0.01);
    EXPECT_NEAR(CadenceController::ReserveMs(6, 60.0), 100.0, 0.01);
    EXPECT_NEAR(CadenceController::ReserveMs(12, 120.0), 100.0, 0.01);
    // Degenerate inputs report zero rather than dividing by zero.
    EXPECT_EQ(CadenceController::ReserveMs(0, 24.0), 0.0);
    EXPECT_EQ(CadenceController::ReserveMs(5, 0.0), 0.0);
}

TEST(DecodeAheadPolicy, PoolSlotCapMakesTwoHundredMsUnreachableAtHighFps) {
    // 7 usable slots (pool is 8, one is always in flight).
    EXPECT_GT(CadenceController::ReserveMs(7, 24.0), 200.0);   // ~291 ms, target met
    EXPECT_LT(CadenceController::ReserveMs(7, 60.0), 200.0);   // ~117 ms, capped
    EXPECT_LT(CadenceController::ReserveMs(7, 120.0), 200.0);  // ~58 ms, capped
}

TEST(DecodeAheadPolicy, BurstsBelowLowWaterOnlyWhenBurstEnabled) {
    const double src = 1000.0 / 24.0;  // 24 fps

    // The shipped default enables the refill burst (scheduler precondition
    // fixed 2026-10-17; A/B in the DecodeAheadPolicy docs). Rollback is an
    // explicit maxBurst = 1.0.
    CadenceController::DecodeAheadPolicy def;
    EXPECT_DOUBLE_EQ(def.maxBurst, 2.0);

    // Burst DISABLED: the decoder paces at the source rate regardless of
    // reserve depth.
    CadenceController::DecodeAheadPolicy off;
    off.maxBurst = 1.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 0.0, off), src, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 50.0, off), src, 0.001);

    // Burst ENABLED: below the low-water mark it doubles up.
    CadenceController::DecodeAheadPolicy on;
    on.maxBurst = 2.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 0.0, on), src / 2.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 50.0, on), src / 2.0, 0.001);
    // At or above the low-water mark it returns to the source rate…
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 100.0, on), src, 0.001);
    // …until the high-water mark, past which it parks (P0-4 backpressure).
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 200.0, on), src, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 250.0, on), src * 4.0, 0.001);
}

TEST(DecodeAheadPolicy, BurstCeilingIsRespected) {
    CadenceController::DecodeAheadPolicy p;
    p.maxBurst = 4.0;
    const double src = 16.667;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 0.0, p), src / 4.0, 0.001);
    // A nonsense ceiling must never make the decoder run SLOWER than realtime.
    p.maxBurst = 0.5;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 0.0, p), src, 0.001);
}

TEST(DecodeAheadPolicy, NeverParksWhenStarvedOrInBand) {
    // P0-4: parking is allowed ONLY above the high-water mark. A starved
    // playhead (negative lead) or an in-band reserve must never decode
    // slower than realtime — throttling there was the reverted 0.6x
    // regression.
    CadenceController::DecodeAheadPolicy p;  // low 100 ms, high 200 ms
    for (double reserve = -400.0; reserve <= 200.0; reserve += 25.0) {
        const double interval = CadenceController::DecodeIntervalMs(16.667, reserve, p);
        EXPECT_LE(interval, 16.667 + 1e-9)
            << "reserve=" << reserve << " must not slow the decoder below 1x";
    }
}

TEST(DecodeAheadPolicy, ParksAboveHighWater) {
    // P0-4: a lead past high-water parks the decoder (4x interval) so the
    // playhead — not an ever-growing lead — bounds the ring. Independent of
    // the burst flag (parking is not bursting).
    CadenceController::DecodeAheadPolicy off;
    off.maxBurst = 1.0;  // isolate parking from the (default-on) refill burst
    CadenceController::DecodeAheadPolicy on;
    on.maxBurst = 2.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, 200.01, off),
                16.667 * 4.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, 500.0, off),
                16.667 * 4.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, 500.0, on),
                16.667 * 4.0, 0.001);
    // At exactly high-water it still runs at source rate (boundary inclusive).
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, 200.0, off),
                16.667, 0.001);
}

TEST(DecodeAheadPolicy, BehindPlayheadBurstsAndNeverParks) {
    // Negative lead = decode is behind the playhead (starved): catch-up
    // burst applies ONLY via catchUpBurst now — maxBurst alone paces at the
    // source rate behind the playhead (it governs refill, not catch-up).
    // Default policy ships catchUpBurst=2.0, so a default policy bursts
    // behind; an explicit 1.0 forces plain source-rate pacing (rollback).
    CadenceController::DecodeAheadPolicy on;
    on.maxBurst = 2.0;
    on.catchUpBurst = 1.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, -50.0, on),
                16.667, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, -10000.0, on),
                16.667, 0.001);
    CadenceController::DecodeAheadPolicy defOff;
    defOff.catchUpBurst = 1.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, -50.0, defOff),
                16.667, 0.001);
    CadenceController::DecodeAheadPolicy def;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, -50.0, def),
                16.667 / 2.0, 0.001);
}

TEST(DecodeAheadPolicy, CatchUpBurstAppliesOnlyBehindPlayhead) {
    // catchUpBurst=2: behind (negative) paces 2x; at/above water the refill
    // and park behavior is byte-identical to burst-off (no deep reserve).
    CadenceController::DecodeAheadPolicy cu;
    cu.maxBurst = 1.0;  // isolate catch-up from the (default-on) refill burst
    cu.catchUpBurst = 2.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(8.333, -50.0, cu),
                8.333 / 2.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(8.333, -10000.0, cu),
                8.333 / 2.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(8.333, 0.0, cu),
                8.333, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(8.333, 10.0, cu),
                8.333, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(8.333, 500.0, cu),
                8.333 * 4.0, 0.001);  // parks above high water, unchanged
}

TEST(DecodeAheadPolicy, ShallowDeficitsCatchUp) {
    // Regression (2026-10-17 live repro): holding 1x for deficits within one
    // source frame is an ABSORBING state — behind the playhead 1x equals the
    // playhead speed, so the deficit can never shrink. Cold-load entries
    // (measured -5..-40 ms) stayed pinned for minutes with late-frame storms
    // and dwell smear; only an external pause/resume reset escaped. Every
    // negative lead must catch up.
    CadenceController::DecodeAheadPolicy cu;
    cu.catchUpBurst = 2.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(8.333, -8.332, cu),
                8.333 / 2.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, -16.0, cu),
                16.667 / 2.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, -0.5, cu),
                16.667 / 2.0, 0.001);
}

TEST(DecodeAheadPolicy, NegativeReserveRecoversToLowWater) {
    // Fixed-point simulation of the live pacing loop (24 fps): each decode
    // iteration takes `interval`, during which the playhead advances one
    // source interval, so the reserve gains (sourceInterval - interval).
    // From the measured cold-load entry deficit the reserve must reach the
    // low-water mark; under the old one-frame hysteresis it froze at the
    // entry value forever (the absorbing state).
    const double src = 1000.0 / 24.0;  // 41.667 ms
    const CadenceController::DecodeAheadPolicy p =
        CadenceController::PolicyFor(24.0);
    double reserve = -40.0;  // measured cold-load entry, 2026-10-17 session
    int iterations = 0;
    while (reserve < p.lowWaterMs && iterations < 240) {
        reserve += src - CadenceController::DecodeIntervalMs(src, reserve, p);
        ++iterations;
    }
    EXPECT_GE(reserve, p.lowWaterMs) << "reserve stayed stuck at entry";
    EXPECT_LE(iterations, 12) << "recovery must complete in ~0.5 s";
}

TEST(DecodeAheadPolicy, NanReserveIsSourceRate) {
    // A torn/racy lead sample must degrade to plain source-rate pacing,
    // never to burst (overspeed) or park (stall).
    CadenceController::DecodeAheadPolicy on;
    on.maxBurst = 2.0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(16.667, nan, on),
                16.667, 0.001);
}
