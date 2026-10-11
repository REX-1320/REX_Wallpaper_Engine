// ═══════════════════════════════════════════════════════════════════════════
// Occlusion-park tests: vendor-neutral idle optimization
// ═══════════════════════════════════════════════════════════════════════════
//
// When every present path reports DXGI_STATUS_OCCLUDED (DWM drops the frame
// because the wallpaper is fully covered — maximized window, lock screen,
// Task View), the engine keeps decoding + drawing + presenting at full rate
// into the void. OcclusionPolicy turns that signal into a park decision:
//
//   - visible samples never park;
//   - all-paths-occluded for >= threshold (default 1s) parks ONCE;
//   - transient occlusion below the threshold never parks;
//   - any visible sample resets the timer; a visible sample while parked
//     resumes ONCE, then stays visible;
//   - the policy starts DISARMED (fresh LOAD grace) and only counts time
//     after Arm() — spurious early OCCLUDEDs can never park playback;
//   - AllPathsOccluded aggregates main + per-monitor compositor states:
//     partial occlusion (any path visible) never parks decode.
//
// OcclusionTracker is the renderer seam: it folds raw present HRESULTs into
// a sticky per-path occluded flag. Only the documented DXGI success code
// counts — real failures (E_FAIL, DEVICE_REMOVED) are recovery's business
// and must NEVER park. No vendor headers, no driver ioctls: the HRESULT
// constants are matched by value so this layer builds anywhere DXGI runs
// (AMD / NVIDIA / Intel behave identically here by API contract).

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "wallpaper/engine/OcclusionPolicy.hpp"

using namespace rex::wallpaper;

namespace {

// Documented DXGI values, matched by value (no dxgi.h needed here).
constexpr long kPresentOk = 0L;                 // S_OK
constexpr long kPresentOccluded = 0x087A0001L;  // DXGI_STATUS_OCCLUDED
constexpr long kPresentFailed = 0x80004005L;    // E_FAIL
constexpr long kDeviceRemoved = 0x887A0005L;    // DXGI_ERROR_DEVICE_REMOVED

}  // namespace

TEST(OcclusionTrackerTest, StartsVisible) {
    OcclusionTracker tracker;
    EXPECT_FALSE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 0u);
}

TEST(OcclusionTrackerTest, SuccessfulPresentStaysVisible) {
    OcclusionTracker tracker;
    tracker.NotifyPresent(kPresentOk);
    tracker.NotifyPresent(kPresentOk);
    EXPECT_FALSE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 0u);
}

TEST(OcclusionTrackerTest, OccludedPresentMarksPathOccluded) {
    OcclusionTracker tracker;
    tracker.NotifyPresent(kPresentOccluded);
    EXPECT_TRUE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 1u);
}

TEST(OcclusionTrackerTest, ConsecutiveOccludedAccumulates) {
    OcclusionTracker tracker;
    tracker.NotifyPresent(kPresentOccluded);
    tracker.NotifyPresent(kPresentOccluded);
    tracker.NotifyPresent(kPresentOccluded);
    EXPECT_TRUE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 3u);
}

TEST(OcclusionTrackerTest, VisiblePresentClearsOcclusion) {
    OcclusionTracker tracker;
    tracker.NotifyPresent(kPresentOccluded);
    tracker.NotifyPresent(kPresentOccluded);
    tracker.NotifyPresent(kPresentOk);
    EXPECT_FALSE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 0u);
}

TEST(OcclusionTrackerTest, RealFailuresNeverCountAsOcclusion) {
    OcclusionTracker tracker;
    tracker.NotifyPresent(kPresentFailed);
    EXPECT_FALSE(tracker.IsOccluded());
    tracker.NotifyPresent(kDeviceRemoved);
    EXPECT_FALSE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 0u);
}

TEST(OcclusionTrackerTest, FailureAfterOcclusionClearsStickyFlag) {
    OcclusionTracker tracker;
    tracker.NotifyPresent(kPresentOccluded);
    ASSERT_TRUE(tracker.IsOccluded());
    // A real failure ends the occlusion run: DWM is no longer dropping
    // frames, the device path is — recovery owns what happens next.
    tracker.NotifyPresent(kPresentFailed);
    EXPECT_FALSE(tracker.IsOccluded());
    EXPECT_EQ(tracker.ConsecutiveOccluded(), 0u);
}

TEST(AllPathsOccludedTest, MainAloneDecidesWhenNoCompositors) {
    EXPECT_TRUE(AllPathsOccluded(true, {}));
    EXPECT_FALSE(AllPathsOccluded(false, {}));
}

TEST(AllPathsOccludedTest, EveryPathMustBeOccluded) {
    EXPECT_TRUE(AllPathsOccluded(true, {true, true}));
    EXPECT_FALSE(AllPathsOccluded(true, {true, false}));
    EXPECT_FALSE(AllPathsOccluded(false, {true, true}));
    EXPECT_FALSE(AllPathsOccluded(true, {false}));
}

TEST(OcclusionPolicyTest, VisibleSamplesNeverPark) {
    OcclusionPolicy policy;
    policy.Arm();
    for (int i = 0; i < 120; ++i) {
        EXPECT_EQ(policy.Update(false, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
}

TEST(OcclusionPolicyTest, SustainedOcclusionParksExactlyOnce) {
    OcclusionPolicy policy(1.0);
    policy.Arm();
    // 0.5s occluded: below threshold, no park.
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
    // Crossing 1.0s cumulative: exactly one Park.
    OcclusionAction saw = OcclusionAction::None;
    for (int i = 0; i < 30; ++i) {
        auto a = policy.Update(true, 1.0 / 60.0);
        if (a == OcclusionAction::Park) EXPECT_EQ(saw, OcclusionAction::None);
        saw = (a == OcclusionAction::Park) ? a : saw;
    }
    EXPECT_EQ(saw, OcclusionAction::Park);
    EXPECT_TRUE(policy.IsParked());
    // Staying occluded while parked: silence, no repeat parks.
    for (int i = 0; i < 60; ++i) {
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_TRUE(policy.IsParked());
}

TEST(OcclusionPolicyTest, TransientOcclusionResetsTimer) {
    OcclusionPolicy policy(1.0);
    policy.Arm();
    for (int i = 0; i < 50; ++i) {  // 0.83s occluded...
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_EQ(policy.Update(false, 1.0 / 60.0), OcclusionAction::None);  // ...peek...
    for (int i = 0; i < 50; ++i) {  // ...0.83s more: timer restarted, no park.
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
}

TEST(OcclusionPolicyTest, VisibleWhileParkedResumesExactlyOnce) {
    OcclusionPolicy policy(0.5);
    policy.Arm();
    for (int i = 0; i < 40; ++i) policy.Update(true, 1.0 / 60.0);
    ASSERT_TRUE(policy.IsParked());
    EXPECT_EQ(policy.Update(false, 1.0 / 60.0), OcclusionAction::Resume);
    EXPECT_FALSE(policy.IsParked());
    // Stays visible afterwards: no repeat resumes.
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(policy.Update(false, 1.0 / 60.0), OcclusionAction::None);
    }
}

TEST(OcclusionPolicyTest, DisarmedPolicyNeverParks) {
    OcclusionPolicy policy(0.25);
    // No Arm(): even long occlusion must not park (fresh-LOAD grace).
    for (int i = 0; i < 300; ++i) {
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
    // Arming starts the clock; the earlier occlusion run does not count.
    policy.Arm();
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
    for (int i = 0; i < 20; ++i) policy.Update(true, 1.0 / 60.0);
    EXPECT_TRUE(policy.IsParked());
}

TEST(OcclusionPolicyTest, ResetDisarmsAndClearsPark) {
    OcclusionPolicy policy(0.25);
    policy.Arm();
    for (int i = 0; i < 30; ++i) policy.Update(true, 1.0 / 60.0);
    ASSERT_TRUE(policy.IsParked());
    policy.Reset();  // LOAD / STOP / UNLOAD: back to grace.
    EXPECT_FALSE(policy.IsParked());
    for (int i = 0; i < 300; ++i) {
        EXPECT_EQ(policy.Update(true, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
}

TEST(OcclusionPolicyTest, PartialOcclusionNeverParksDecode) {
    OcclusionPolicy policy(0.25);
    policy.Arm();
    // One monitor covered, the other visible: aggregate stays visible.
    for (int i = 0; i < 300; ++i) {
        const bool all = AllPathsOccluded(true, {true, false});
        EXPECT_EQ(policy.Update(all, 1.0 / 60.0), OcclusionAction::None);
    }
    EXPECT_FALSE(policy.IsParked());
}
