// ═══════════════════════════════════════════════════════════════════════════
// PreciseSleepUntil accuracy pin (Phase 2: sleep-quantization fix)
// ═══════════════════════════════════════════════════════════════════════════
//
// Ten 16.67 ms frame paces must cost ~167 ms, not ~310 ms. Plain
// Sleep()/sleep_for() quantizes to the 15.6 ms Windows quantum (16.7 ms
// sleeps wake after ~31 ms → every 60 fps source decodes at 32 fps).
// PreciseSleepUntil uses a high-resolution waitable timer instead.
// This test pins the tool; the decoder wiring is proven live (60 fps
// playback after the switch, 33 fps before).

#include <gtest/gtest.h>

#include <chrono>

#include <shared/PreciseSleep.hpp>

TEST(PreciseSleepTest, TenFramePacesCostTenFramesNotTwenty) {
    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        rex::shared::PreciseSleepUntil(
            t0 + duration<double, std::milli>(16.667 * (i + 1)));
    }
    const double elapsedMs =
        duration<double, std::milli>(steady_clock::now() - t0).count();
    // 167 ms nominal; budget generously for CI jitter. sleep_for-based
    // pacing lands near 310 ms and fails this by a mile.
    EXPECT_GT(elapsedMs, 100.0);
    EXPECT_LT(elapsedMs, 260.0);
}
