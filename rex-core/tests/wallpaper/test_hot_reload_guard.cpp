// ═══════════════════════════════════════════════════════════════════════════
// R1 regression tests: same-thread frame-lock reentry guard
// ═══════════════════════════════════════════════════════════════════════════
//
// BeginFrame() holds m_renderMutex until EndFrame(). LoadTexture() takes the
// same (non-recursive) mutex — called on the render thread inside a frame
// (the old hot-reload site) it self-deadlocks instead of failing. Desired:
// the reentry is detected so the caller fails loudly; cross-thread calls
// still block normally for exclusion; after ExitFrame nobody is flagged.

#include <gtest/gtest.h>

#include <thread>

#include "graphics/renderer/FrameLockPolicy.hpp"

using namespace rex::graphics;

TEST(FrameLockGuardTest, NoFrameNoReentry) {
    FrameLockPolicy guard;
    EXPECT_FALSE(guard.IsReentrantCall());
}

TEST(FrameLockGuardTest, SameThreadInFrameIsReentry) {
    FrameLockPolicy guard;
    guard.EnterFrame();
    EXPECT_TRUE(guard.IsReentrantCall());
    guard.ExitFrame();
    EXPECT_FALSE(guard.IsReentrantCall());
}

TEST(FrameLockGuardTest, CrossThreadInFrameIsNotReentry) {
    // Another thread must still be allowed to block on the mutex for
    // exclusion (e.g. command-thread scene loads during video playback).
    FrameLockPolicy guard;
    guard.EnterFrame();
    bool other = true;
    std::thread t([&]() { other = guard.IsReentrantCall(); });
    t.join();
    EXPECT_FALSE(other);
    guard.ExitFrame();
    EXPECT_FALSE(guard.IsReentrantCall());
}
