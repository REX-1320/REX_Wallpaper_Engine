// ═══════════════════════════════════════════════════════════════════════════
// P0-2 regression tests: display-topology change detection + HDR sync plan
// ═══════════════════════════════════════════════════════════════════════════
//
// G3: CheckDisplayChanges only compared monitor count + primary W/H, so
// secondary-only moves, same-resolution swaps and DPI changes never
// reconciled. R16: the renderer's HDR output flag was snapshot-once at
// Initialize with the HDR PSO created only when starting HDR — a later
// switch had stale state and nothing to bind.

#include <gtest/gtest.h>

#include <vector>

#include "wallpaper/engine/DisplayTopology.hpp"
#include "graphics/renderer/HdrSyncPolicy.hpp"

using namespace rex::wallpaper;
using namespace rex::graphics;

namespace {

MonitorInfo MakeMonitor(const std::string& id, int x, int y, int w, int h,
                        bool primary, float dpi = 1.0f) {
    MonitorInfo m;
    m.id = id;
    m.name = id;
    m.x = x;
    m.y = y;
    m.width = w;
    m.height = h;
    m.isPrimary = primary;
    m.dpiScale = dpi;
    return m;
}

std::vector<MonitorInfo> SoloPrimary() {
    return { MakeMonitor("DP-1", 0, 0, 3840, 2160, true) };
}

}  // namespace

// ─── Topology predicate ─────────────────────────────────────────────────────

TEST(TopologyChangedTest, IdenticalListsAreUnchanged) {
    EXPECT_FALSE(MonitorTopologyChanged(SoloPrimary(), SoloPrimary()));
}

TEST(TopologyChangedTest, MonitorCountChangeIsDetected) {
    auto two = SoloPrimary();
    two.push_back(MakeMonitor("HDMI-1", 3840, 0, 1920, 1080, false));
    EXPECT_TRUE(MonitorTopologyChanged(SoloPrimary(), two));
    EXPECT_TRUE(MonitorTopologyChanged(two, SoloPrimary()));
}

TEST(TopologyChangedTest, SecondaryMoveIsDetected) {
    auto a = SoloPrimary();
    a.push_back(MakeMonitor("HDMI-1", 3840, 0, 1920, 1080, false));
    auto b = a;
    b[1].x = -1920;  // secondary moved left of primary, same count/size
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
}

TEST(TopologyChangedTest, SameResolutionSwapIsDetected) {
    // Same count, same sizes — but a different monitor is primary.
    auto a = SoloPrimary();
    auto b = SoloPrimary();
    b[0].id = "HDMI-1";
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
}

TEST(TopologyChangedTest, DpiChangeIsDetected) {
    auto a = SoloPrimary();
    auto b = SoloPrimary();
    b[0].dpiScale = 1.5f;  // 150% scale, same pixel geometry
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
}

TEST(TopologyChangedTest, PhysicalSwapSameNameIsDetected) {
    // Same device name + geometry (Windows reuses \\.\DISPLAY1) but a
    // different physical panel: the EDID hash is the only witness.
    auto a = SoloPrimary();
    a[0].edidHash = 0x1234;
    auto b = SoloPrimary();
    b[0].edidHash = 0x5678;
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
}

TEST(TopologyChangedTest, AdapterMoveIsDetected) {
    // Same name, geometry, EDID — moved to a different GPU.
    auto a = SoloPrimary();
    a[0].adapterLuid = 0xf956;
    auto b = SoloPrimary();
    b[0].adapterLuid = 0xe29c;
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
}

TEST(TopologyChangedTest, UnknownToKnownAdapterReconciles) {
    // 0 = unresolvable: unknown→known counts as changed (safe direction),
    // unknown→unknown stays steady (no reconcile storm on headless/RDP).
    auto a = SoloPrimary();
    auto b = SoloPrimary();
    b[0].adapterLuid = 0xf956;
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
    EXPECT_FALSE(MonitorTopologyChanged(a, a));
}

TEST(TopologyChangedTest, EmptyToEmptyIsUnchanged) {
    EXPECT_FALSE(MonitorTopologyChanged({}, {}));
}

TEST(TopologyChangedTest, RefreshRateChangeIsDetected) {
    // 160 Hz -> 60 Hz moves no pixels: without Hz in the predicate the
    // cadence controller would never recalculate.
    auto a = SoloPrimary();
    a[0].refreshHz = 160;
    auto b = a;
    b[0].refreshHz = 60;
    EXPECT_TRUE(MonitorTopologyChanged(a, b));
}

// ─── Display-Hz selection (Phase 4) ───────────────────────────────────────

TEST(DisplayHzTest, CurrentMonitorHzWins) {
    auto monitors = SoloPrimary();
    monitors[0].refreshHz = 160;
    EXPECT_EQ(SelectDisplayHz(monitors, "DP-1", 60), 160);
}

TEST(DisplayHzTest, UnknownMonitorFallsBack) {
    auto monitors = SoloPrimary();
    monitors[0].refreshHz = 160;
    EXPECT_EQ(SelectDisplayHz(monitors, "NOPE", 60), 60);
}

TEST(DisplayHzTest, UnreportedHzFallsBack) {
    auto monitors = SoloPrimary();  // refreshHz 0 = unknowable
    EXPECT_EQ(SelectDisplayHz(monitors, "DP-1", 60), 60);
}

TEST(DisplayHzTest, EmptyListFallsBack) {
    EXPECT_EQ(SelectDisplayHz({}, "DP-1", 60), 60);
}

// ─── HDR sync plan ──────────────────────────────────────────────────────────

TEST(HdrSyncPlanTest, SteadySdrNeedsNothing) {
    const auto a = PlanHdrSync(false, false, false);
    EXPECT_FALSE(a.updateFlag);
    EXPECT_FALSE(a.createHdrPso);
}

TEST(HdrSyncPlanTest, SteadyHdrWithPsoNeedsNothing) {
    const auto a = PlanHdrSync(true, true, true);
    EXPECT_FALSE(a.updateFlag);
    EXPECT_FALSE(a.createHdrPso);
}

TEST(HdrSyncPlanTest, SdrToHdrUpdatesFlagAndCreatesPso) {
    // Starting SDR leaves m_pipelineStateHdr null — a later switch must
    // create it, or the PSO choice binds null.
    const auto a = PlanHdrSync(false, true, false);
    EXPECT_TRUE(a.updateFlag);
    EXPECT_TRUE(a.createHdrPso);
}

TEST(HdrSyncPlanTest, HdrToSdrUpdatesFlagOnly) {
    const auto a = PlanHdrSync(true, false, true);
    EXPECT_TRUE(a.updateFlag);
    EXPECT_FALSE(a.createHdrPso);
}

TEST(HdrSyncPlanTest, MissingPsoIsRecreatedEvenWhenFlagMatches) {
    const auto a = PlanHdrSync(true, true, false);
    EXPECT_FALSE(a.updateFlag);
    EXPECT_TRUE(a.createHdrPso);
}
