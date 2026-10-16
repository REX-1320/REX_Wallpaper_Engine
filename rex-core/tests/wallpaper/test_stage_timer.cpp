#include <gtest/gtest.h>

#include <string>

#include "wallpaper/playback/StageTimer.hpp"

using rex::wallpaper::DecodeStage;
using rex::wallpaper::PresentStage;
using rex::wallpaper::StageRing;

TEST(StageRingTest, SnapshotIsOldestFirst) {
    StageRing<DecodeStage, 4> r;
    EXPECT_EQ(r.Size(), 0u);
    EXPECT_TRUE(r.Snapshot().empty());
    for (uint64_t i = 1; i <= 3; ++i) {
        DecodeStage s;
        s.seq = i;
        r.Push(s);
    }
    const auto snap = r.Snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0].seq, 1u);
    EXPECT_EQ(snap[2].seq, 3u);
}

TEST(StageRingTest, OverwriteKeepsLatestNOldestFirst) {
    StageRing<DecodeStage, 4> r;
    for (uint64_t i = 1; i <= 6; ++i) {
        DecodeStage s;
        s.seq = i;
        r.Push(s);
    }
    EXPECT_EQ(r.Size(), 4u);
    const auto snap = r.Snapshot();
    ASSERT_EQ(snap.size(), 4u);
    EXPECT_EQ(snap[0].seq, 3u);
    EXPECT_EQ(snap[3].seq, 6u);
}

TEST(StageRingTest, BackTracksMostRecent) {
    StageRing<PresentStage, 4> r;
    EXPECT_EQ(r.Back(), nullptr);
    PresentStage a;
    a.seq = 7;
    r.Push(a);
    ASSERT_NE(r.Back(), nullptr);
    EXPECT_EQ(r.Back()->seq, 7u);
    r.Clear();
    EXPECT_EQ(r.Size(), 0u);
    EXPECT_EQ(r.Back(), nullptr);
}

TEST(StagePercentileTest, KnownValues) {
    using rex::wallpaper::StagePercentile;
    EXPECT_DOUBLE_EQ(StagePercentile({}, 0.5), 0.0);
    EXPECT_DOUBLE_EQ(StagePercentile({5.0}, 0.99), 5.0);
    // 1..100: p50 ~50.5, p95 ~95.05, max 100.
    std::vector<double> v;
    for (int i = 1; i <= 100; ++i) v.push_back(static_cast<double>(i));
    EXPECT_NEAR(StagePercentile(v, 0.50), 50.5, 1e-9);
    EXPECT_NEAR(StagePercentile(v, 0.95), 95.05, 1e-9);
    EXPECT_NEAR(StagePercentile(v, 1.0), 100.0, 1e-9);
    EXPECT_NEAR(StagePercentile(v, 0.0), 1.0, 1e-9);
}

TEST(StageCsvTest, HeadersAndRowsAreStable) {
    // Analysis tooling parses by position: pin the column order here so a
    // reorder breaks this test instead of silently corrupting data.
    EXPECT_STREQ(rex::wallpaper::DecodeStageCsvHeader(),
                 "seq,frameIndex,demuxUs,sendUs,recvUs,convUs,arrivalUs,queueDepth");
    DecodeStage d{};
    d.seq = 9;
    d.queueDepth = 3;
    EXPECT_EQ(rex::wallpaper::DecodeStageCsvRow(d), "9,0,0,0,0,0,0,3");
    EXPECT_STREQ(
        rex::wallpaper::PresentStageCsvHeader(),
        "seq,frameCount,beginWaitMs_x1000,drawMs_x1000,endMs_x1000,"
        "gapMs_x1000,targetMs_x1000,dwell,presentedNew,deadlineMiss");
    PresentStage p{};
    p.seq = 4;
    p.deadlineMiss = 1;
    EXPECT_EQ(rex::wallpaper::PresentStageCsvRow(p),
              "4,0,0,0,0,0,0,0,0,1");
}
