// DisplayCadenceStats — the clock REX was missing.
//
// REX measures when it SUBMITS. The roadmap requires measuring when frames
// actually REACH the panel and for how many refreshes. DXGI
// (IDXGISwapChain::GetFrameStatistics) supplies that:
//   PresentRefreshCount = refreshes the frame was displayed for
//   SyncQPCTime         = timestamp of the sync
//
// The accumulator is pure so it is testable with no swap chain, GPU or display —
// which matters because the first PresentMon attempt in this project produced a
// convincing but false result (wall_research.md P10) purely from misreading a
// tool. A pure, unit-tested accumulator cannot be misread that way.

#include <gtest/gtest.h>

#include "graphics/renderer/DisplayCadenceStats.hpp"

using rex::graphics::DisplayCadenceStats;

namespace {

DisplayCadenceStats::Sample S(uint32_t dwell, int64_t qpc, bool valid = true) {
    DisplayCadenceStats::Sample s;
    s.presentRefreshCount = dwell;
    s.syncQpc = qpc;
    s.valid = valid;
    return s;
}

}  // namespace

TEST(DisplayCadence, EmptyStatsReportNothing) {
    DisplayCadenceStats st;
    EXPECT_EQ(st.Samples(), 0u);
    EXPECT_DOUBLE_EQ(st.PercentileDwell(0.5), 0.0);
    EXPECT_DOUBLE_EQ(st.PercentileGapMicros(0.5), 0.0);
    EXPECT_FALSE(st.HasCadence());
}

TEST(DisplayCadence, InvalidSamplesAreIgnored) {
    DisplayCadenceStats st;
    st.Add(S(0, 0, /*valid=*/false));
    EXPECT_EQ(st.Samples(), 0u);
}

TEST(DisplayCadence, DwellIsReportedPerFrame) {
    DisplayCadenceStats st;
    for (int i = 0; i < 300; ++i) {
        // The 24fps-on-160Hz signature: 6 and 7 vblanks alternating.
        st.Add(S((i % 2) ? 7u : 6u, static_cast<int64_t>(i) * 1000));
    }
    EXPECT_EQ(st.Samples(), 300u);
    EXPECT_TRUE(st.HasCadence());
    EXPECT_DOUBLE_EQ(st.PercentileDwell(0.0), 6.0);
    EXPECT_DOUBLE_EQ(st.PercentileDwell(1.0), 7.0);
    // Both dwells present => the source rate is not an integer multiple of the
    // refresh rate. That is the judder, measured.
    EXPECT_LT(st.PercentileDwell(0.0), st.PercentileDwell(1.0));
}

TEST(DisplayCadence, SteadyDwellHasNoSpread) {
    DisplayCadenceStats st;
    for (int i = 0; i < 300; ++i) st.Add(S(6u, static_cast<int64_t>(i) * 1000));
    EXPECT_DOUBLE_EQ(st.PercentileDwell(0.0), 6.0);
    EXPECT_DOUBLE_EQ(st.PercentileDwell(1.0), 6.0);
}

TEST(DisplayCadence, DisplayedGapsComeFromSyncQpcDeltas) {
    DisplayCadenceStats st;
    // 10 ms per frame. Steps are raw QPC ticks; the getter reports MICROS
    // (audit 5.6 #12), so the expectation converts with the same factor.
    constexpr int64_t kStepTicks = 10000;
    for (int i = 0; i < 300; ++i) {
        st.Add(S(1u, static_cast<int64_t>(i) * kStepTicks));
    }
    EXPECT_DOUBLE_EQ(st.PercentileGapMicros(0.5),
                     DisplayCadenceStats::TicksToMicros(kStepTicks));
}

TEST(DisplayCadence, TicksToMicrosUsesTheRealFrequency) {
    const double hz = DisplayCadenceStats::QpcHz();
    ASSERT_GT(hz, 0.0);
    // One full second of ticks must read as exactly 1e6 micros.
    EXPECT_DOUBLE_EQ(
        DisplayCadenceStats::TicksToMicros(static_cast<int64_t>(hz)), 1000000.0);
}

TEST(DisplayCadence, JitterInDeltasShowsInTheTail) {
    DisplayCadenceStats st;
    // 290 steady frames then one 50% larger step.
    for (int i = 0; i < 300; ++i) {
        st.Add(S(1u, static_cast<int64_t>(i) * 10000 + (i >= 290 ? 5000 : 0)));
    }
    EXPECT_DOUBLE_EQ(st.PercentileGapMicros(0.5),
                     DisplayCadenceStats::TicksToMicros(10000));
    EXPECT_GT(st.PercentileGapMicros(1.0),
              DisplayCadenceStats::TicksToMicros(10000));
}

TEST(DisplayCadence, ResetClearsEverything) {
    DisplayCadenceStats st;
    for (int i = 0; i < 300; ++i) st.Add(S(6u, static_cast<int64_t>(i) * 1000));
    EXPECT_TRUE(st.HasCadence());
    st.Reset();
    EXPECT_EQ(st.Samples(), 0u);
    EXPECT_FALSE(st.HasCadence());
    EXPECT_DOUBLE_EQ(st.PercentileDwell(0.5), 0.0);
}

TEST(DisplayCadence, RingWrapsWithoutCorruptingRecentSamples) {
    DisplayCadenceStats st;
    // Feed 2*kRingSize samples; only the most recent ring may be visible.
    const int total = static_cast<int>(DisplayCadenceStats::kRingSize) * 2;
    for (int i = 0; i < total; ++i) {
        st.Add(S(static_cast<uint32_t>(i % 8), static_cast<int64_t>(i) * 1000));
    }
    EXPECT_TRUE(st.HasCadence());
    // Every retained dwell must be in the range we fed (0..7).
    EXPECT_GE(st.PercentileDwell(0.0), 0.0);
    EXPECT_LE(st.PercentileDwell(1.0), 7.0);
}