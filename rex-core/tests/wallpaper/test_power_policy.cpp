// ═══════════════════════════════════════════════════════════════════════════
// P3-G1 regression tests: battery-saver pause rule
// ═══════════════════════════════════════════════════════════════════════════
//
// Laptops decoded/uploaded/presented at full cadence on battery. The engine
// pauses expensive wallpaper work when the OS reports battery saver — or a
// critically low battery off AC — and resumes when clear. Unknown states
// never pause (fail-open).

#include <gtest/gtest.h>

#include "wallpaper/engine/PowerPolicy.hpp"

using namespace rex::wallpaper;

TEST(PowerPauseRuleTest, AcPowerNeverPauses) {
    EXPECT_FALSE(ShouldPauseForPower(true, false, 100));
    EXPECT_FALSE(ShouldPauseForPower(true, false, 5));
    // Saver on AC still pauses (covered by BatterySaverPauses).
}

TEST(PowerPauseRuleTest, BatterySaverPauses) {
    EXPECT_TRUE(ShouldPauseForPower(true, true, 100));
    EXPECT_TRUE(ShouldPauseForPower(false, true, 50));
}

TEST(PowerPauseRuleTest, CriticalBatteryOffAcPauses) {
    EXPECT_TRUE(ShouldPauseForPower(false, false, 15));
    EXPECT_TRUE(ShouldPauseForPower(false, false, 5));
}

TEST(PowerPauseRuleTest, HealthyBatteryOffAcKeepsRunning) {
    EXPECT_FALSE(ShouldPauseForPower(false, false, 80));
    EXPECT_FALSE(ShouldPauseForPower(false, false, 16));
}

TEST(PowerPauseRuleTest, UnknownStatesFailOpen) {
    // AC unknown (255) reads as online; percent unknown (255) reads as -1.
    EXPECT_FALSE(ShouldPauseForPower(true, false, -1));
}
