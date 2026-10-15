// ═══════════════════════════════════════════════════════════════════════════
// CadenceController table tests (Phase 3)
// ═══════════════════════════════════════════════════════════════════════════
//
// presentationTarget = min(sourceFps × rate, displayHz, userCap) is the
// MAXIMUM USEFUL presentation cadence — never a decoder throttle.

#include <gtest/gtest.h>

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

    // Burst DISABLED (the shipped default while the scheduler work settles):
    // the decoder paces at the source rate regardless of reserve depth.
    CadenceController::DecodeAheadPolicy off;
    EXPECT_DOUBLE_EQ(off.maxBurst, 1.0);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 0.0, off), src, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 50.0, off), src, 0.001);

    // Burst ENABLED: below the low-water mark it doubles up.
    CadenceController::DecodeAheadPolicy on;
    on.maxBurst = 2.0;
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 0.0, on), src / 2.0, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 50.0, on), src / 2.0, 0.001);
    // At or above the low-water mark it returns to the source rate.
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 100.0, on), src, 0.001);
    EXPECT_NEAR(CadenceController::DecodeIntervalMs(src, 250.0, on), src, 0.001);
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

TEST(DecodeAheadPolicy, NeverDecodesSlowerThanRealtime) {
    CadenceController::DecodeAheadPolicy p;
    for (double reserve = 0.0; reserve <= 400.0; reserve += 25.0) {
        const double interval = CadenceController::DecodeIntervalMs(16.667, reserve, p);
        EXPECT_LE(interval, 16.667 + 1e-9)
            << "reserve=" << reserve << " must not slow the decoder below 1x";
    }
}
