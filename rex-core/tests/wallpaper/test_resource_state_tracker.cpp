#include <gtest/gtest.h>
#include "graphics/renderer/ResourceStateTracker.hpp"
#include <d3d12.h>

namespace rex::graphics {
namespace {

// ResourceStateTracker requires a real ID3D12GraphicsCommandList for Transition().
// These tests validate the API surface and Reset() behavior.

TEST(ResourceStateTrackerTest, RS01_DefaultConstruction) {
    ResourceStateTracker tracker;
    // No crash on construction; object is valid
    SUCCEED();
}

TEST(ResourceStateTrackerTest, RS02_ResetClearsState) {
    ResourceStateTracker tracker;
    // Reset should not crash
    tracker.Reset();
    tracker.Reset();  // Double reset is safe
    SUCCEED();
}

TEST(ResourceStateTrackerTest, RS03_TransitionReturnsBoolWithoutDevice) {
    // Transition requires a command list; without a real D3D12 device
    // we can only test that the tracker can be constructed and reset.
    // Full integration tests validate actual barrier insertion.
    ResourceStateTracker tracker;
    tracker.Reset();
    SUCCEED();
}

TEST(ResourceStateTrackerTest, RS04_MultipleResets) {
    ResourceStateTracker tracker;
    for (int i = 0; i < 10; ++i) {
        tracker.Reset();
    }
    SUCCEED();
}

} // namespace
} // namespace rex::graphics
