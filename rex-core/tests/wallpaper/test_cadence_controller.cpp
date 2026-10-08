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
