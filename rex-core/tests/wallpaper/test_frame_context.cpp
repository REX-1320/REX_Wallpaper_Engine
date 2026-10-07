// ═══════════════════════════════════════════════════════════════════════════
// P0-4 regression tests: frame-context reset gating (R17)
// ═══════════════════════════════════════════════════════════════════════════
//
// BeginFrame unconditionally Reset() the sole command allocator while the
// prior frame could still be in flight (the DXGI latency waitable is not a
// GPU-completion fence). A hitch >1 vsync then reset in-flight command
// memory — a D3D12 spec violation. Fix: a FrameContext ring where a
// context is reset only after its fence value completed.

#include <gtest/gtest.h>

#include "graphics/renderer/FrameContextPolicy.hpp"

using namespace rex::graphics;

TEST(FrameContextReadyTest, NeverUsedContextIsReady) {
    // Fresh contexts carry fence 0; the queue starts at 0 — reset allowed.
    EXPECT_TRUE(FrameContextReady(0, 0));
}

TEST(FrameContextReadyTest, CompletedContextIsReady) {
    EXPECT_TRUE(FrameContextReady(7, 7));
    EXPECT_TRUE(FrameContextReady(7, 9));
}

TEST(FrameContextReadyTest, InFlightContextIsNotReady) {
    // The GPU has not reached this context's fence: Reset() here would
    // recycle command memory still being executed.
    EXPECT_FALSE(FrameContextReady(9, 7));
    EXPECT_FALSE(FrameContextReady(1, 0));
}

TEST(FrameRingTest, RingIndexesCycleInOrder) {
    EXPECT_EQ(FrameContextIndex(0, 3), 0u);
    EXPECT_EQ(FrameContextIndex(1, 3), 1u);
    EXPECT_EQ(FrameContextIndex(2, 3), 2u);
    EXPECT_EQ(FrameContextIndex(3, 3), 0u);
    EXPECT_EQ(FrameContextIndex(4, 3), 1u);
}
