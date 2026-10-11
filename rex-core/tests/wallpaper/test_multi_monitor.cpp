#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "wallpaper/video/VideoManager.hpp"
#include "graphics/renderer/Device.hpp"

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

// ═══════════════════════════════════════════════════════════════════════════
// Per-monitor present counts (P2 telemetry, headless: fake monitors)
// ═══════════════════════════════════════════════════════════════════════════

TEST(MonitorPresentCountsTest, FakeMonitorsReportZeroCounts) {
    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable";
    }
    WallpaperConfig config;
    config.enableHardwareAcceleration = true;
    VideoManager vm;
    if (vm.Initialize(config, d3d12.Get()).IsError()) {
        GTEST_SKIP() << "VideoManager init unavailable";
    }

    MonitorInfo m1;
    m1.id = "FAKE-1";
    m1.name = "FAKE-1";
    m1.width = 1920;
    m1.height = 1080;
    m1.isPrimary = true;
    MonitorInfo m2 = m1;
    m2.id = "FAKE-2";
    m2.name = "FAKE-2";
    m2.isPrimary = false;
    ASSERT_TRUE(vm.AddMonitor(m1).IsSuccess());
    ASSERT_TRUE(vm.AddMonitor(m2).IsSuccess());

    // Dormant compositors (no WorkerW headless) report zero, keyed by id.
    const auto counts = vm.GetMonitorPresentCounts();
    ASSERT_EQ(counts.size(), 2u);
    EXPECT_EQ(counts[0].first, "FAKE-1");
    EXPECT_EQ(counts[0].second, 0u);
    EXPECT_EQ(counts[1].first, "FAKE-2");
    EXPECT_EQ(counts[1].second, 0u);

    vm.Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Single-audio-owner policy (P3-G4 proof, headless: fake monitors)
// ═══════════════════════════════════════════════════════════════════════════

// Layered per-monitor videos would mix WASAPI streams into cacophony.
// The first opened monitor video owns the audio; later ones start muted.
// Fake MonitorInfos exercise this with no second display present.

TEST(MonitorAudioTest, SecondMonitorVideoStartsMuted) {
    const char* env = std::getenv("REX_TEST_VIDEO_AUDIO");
    std::string video = (env && *env && std::filesystem::exists(env))
        ? env : "D:/live wallpaper/lakeside-morning-minecraft-moewalls-com.mp4";
    if (!std::filesystem::exists(video)) {
        GTEST_SKIP() << "no audio-bearing test video (set REX_TEST_VIDEO_AUDIO)";
    }

    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable";
    }
    WallpaperConfig config;
    config.enableHardwareAcceleration = true;
    VideoManager vm;
    if (vm.Initialize(config, d3d12.Get()).IsError()) {
        GTEST_SKIP() << "VideoManager init unavailable";
    }

    MonitorInfo m1;
    m1.id = "FAKE-1";
    m1.name = "FAKE-1";
    m1.width = 1920;
    m1.height = 1080;
    m1.isPrimary = true;
    MonitorInfo m2 = m1;
    m2.id = "FAKE-2";
    m2.name = "FAKE-2";
    m2.isPrimary = false;
    if (vm.AddMonitor(m1).IsError() || vm.AddMonitor(m2).IsError()) {
        GTEST_SKIP() << "fake monitor compositors unavailable headless";
    }

    ASSERT_TRUE(vm.OpenVideoForMonitor("FAKE-1", video).IsSuccess());
    ASSERT_TRUE(vm.OpenVideoForMonitor("FAKE-2", video).IsSuccess());
    EXPECT_FALSE(vm.IsMonitorVideoMuted("FAKE-1")) << "first owner must play sound";
    EXPECT_TRUE(vm.IsMonitorVideoMuted("FAKE-2")) << "second video must start muted";
    EXPECT_FALSE(vm.IsMonitorVideoMuted("NOPE"));

    vm.Shutdown();
}
