#include <gtest/gtest.h>
#include "wallpaper/playback/FrameScheduler.hpp"
#include <algorithm>
#include <thread>
#include <atomic>

namespace rex::wallpaper {
namespace {

TEST(FrameSchedulerTest, FS01_FirstFrameIsNotSkipped) {
    FrameScheduler scheduler;
    VideoFrame frame{};
    frame.valid = true;
    frame.slot = 0;
    frame.frameIndex = 1;
    scheduler.Push(frame);

    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->valid);
    EXPECT_EQ(result->frameIndex, 1u);
}

TEST(FrameSchedulerTest, FS02_LatestWinsSemantics) {
    FrameScheduler scheduler;

    VideoFrame frameA{};
    frameA.valid = true;
    frameA.slot = 0;
    frameA.frameIndex = 1;
    scheduler.Push(frameA);

    VideoFrame frameB{};
    frameB.valid = true;
    frameB.slot = 1;
    frameB.frameIndex = 2;
    scheduler.Push(frameB);

    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 2u);
}

TEST(FrameSchedulerTest, FS03_NoNewFrameReturnsNullptr) {
    FrameScheduler scheduler;
    VideoFrame frame{};
    frame.valid = true;
    frame.slot = 0;
    frame.frameIndex = 1;
    scheduler.Push(frame);

    const VideoFrame* first = scheduler.Tick(0.0);
    ASSERT_NE(first, nullptr);

    const VideoFrame* second = scheduler.Tick(0.0);
    EXPECT_EQ(second, nullptr);
}

TEST(FrameSchedulerTest, FS04_RingWrapAround) {
    FrameScheduler scheduler;
    // Push more frames than the ring size (kNumSlots = 8)
    for (int i = 0; i < 10; ++i) {
        VideoFrame frame{};
        frame.valid = true;
        frame.slot = i % 8;
        frame.frameIndex = i + 1;
        scheduler.Push(frame);
    }

    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 10u);
}

TEST(FrameSchedulerTest, FS05_ThreadSafety) {
    FrameScheduler scheduler;
    std::atomic<int> pushCount{0};
    std::atomic<int> tickCount{0};

    std::thread producer([&]() {
        for (int i = 0; i < 1000; ++i) {
            VideoFrame frame{};
            frame.valid = true;
            frame.slot = i % 8;
            frame.frameIndex = i + 1;
            scheduler.Push(frame);
            pushCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&]() {
        for (int i = 0; i < 1000; ++i) {
            scheduler.Tick(0.0);
            tickCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    // No crash means success; verify both threads ran
    EXPECT_EQ(pushCount.load(), 1000);
    EXPECT_EQ(tickCount.load(), 1000);
}

} // namespace
} // namespace rex::wallpaper
