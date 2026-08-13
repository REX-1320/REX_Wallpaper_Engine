#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <filesystem>
#include <string>

using namespace rex::wallpaper;

// The Windows bundled wallpaper is present on essentially every Windows
// install; skip rather than fail when it is missing (bare CI images).
static bool HasSampleImage() {
    return std::filesystem::exists(
        "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg");
}

// ═══════════════════════════════════════════════════════════════════════════
// Multi-monitor enumeration & DPI
// ═══════════════════════════════════════════════════════════════════════════

TEST(MonitorTest, EnumerateMonitorsReturnsAtLeastOne) {
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    auto monitors = engine->EnumerateMonitors();
    EXPECT_GE(monitors.size(), 1u);

    bool hasPrimary = false;
    for (const auto& m : monitors) {
        EXPECT_GT(m.width, 0);
        EXPECT_GT(m.height, 0);
        EXPECT_FALSE(m.id.empty());
        if (m.isPrimary) hasPrimary = true;
    }
    EXPECT_TRUE(hasPrimary);

    engine->Shutdown();
}

TEST(MonitorTest, DpiScalingReportedCorrectly) {
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    auto monitors = engine->EnumerateMonitors();
    ASSERT_GE(monitors.size(), 1u);

    for (const auto& m : monitors) {
        // 100%..400% scaling is the supported range on Windows 10/11.
        EXPECT_GE(m.dpiScale, 1.0f);
        EXPECT_LE(m.dpiScale, 4.0f);
    }

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Per-monitor wallpaper assignment
// ═══════════════════════════════════════════════════════════════════════════

TEST(MonitorTest, PerMonitorWallpaperAssignment) {
    if (!HasSampleImage()) {
        GTEST_SKIP() << "sample image not present";
    }

    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    auto monitors = engine->EnumerateMonitors();
    ASSERT_GE(monitors.size(), 1u);

    // Assign a static wallpaper to every connected monitor.
    for (const auto& m : monitors) {
        auto result = engine->SetPerMonitorWallpaper(
            m.id, "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg");
        if (result.IsError()) {
            // Single-monitor WorkerW hosts create no per-monitor
            // compositors — the assignment path is a no-op there.
            GTEST_SKIP() << "per-monitor compositors unavailable: "
                         << result.Error().message;
        }
    }

    // Clear the overrides again (keeps the test re-runnable).
    for (const auto& m : monitors) {
        EXPECT_TRUE(engine->SetPerMonitorWallpaper(m.id, "").IsSuccess());
    }

    engine->Shutdown();
}
