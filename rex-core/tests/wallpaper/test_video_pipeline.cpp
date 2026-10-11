#include "wallpaper/video/VideoManager.hpp"

#include "graphics/renderer/Device.hpp"

#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <gtest/gtest.h>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace rex::wallpaper {
namespace {

// The sample video is machine-specific; override via REX_TEST_VIDEO.
std::string FindTestVideo() {
    if (const char* env = std::getenv("REX_TEST_VIDEO")) {
        if (*env && std::filesystem::exists(env)) return env;
    }
    const std::string fallback =
        "C:/Users/munna/Videos/Screen Recordings/"
        "Screen Recording 2026-07-24 225314.mp4";
    return std::filesystem::exists(fallback) ? fallback : std::string();
}

TEST(VideoPipeline, HardwareDecodeProducesSharedFrames) {
    const std::string path = FindTestVideo();
    if (path.empty()) {
        GTEST_SKIP() << "no test video found (set REX_TEST_VIDEO)";
    }

    // D3D12 device — the video manager creates its D3D11 device on the
    // same adapter (same-adapter requirement for shared textures/fences).
    rex::graphics::Device d3d12;
    const auto devResult = d3d12.Initialize();
    if (devResult.IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable: " << devResult.Error().message;
    }

    WallpaperConfig config;
    config.enableHardwareAcceleration = true;

    VideoManager vm;
    const auto initResult = vm.Initialize(config, d3d12.Get());
    if (initResult.IsError()) {
        GTEST_SKIP() << "VideoManager init unavailable: " << initResult.Error().message;
    }
    ASSERT_TRUE(vm.IsInitialized());

    const auto openResult = vm.OpenVideo(path);
    ASSERT_TRUE(openResult.IsSuccess()) << openResult.Error().message;
    ASSERT_TRUE(vm.IsOpen());
    EXPECT_GT(vm.GetWidth(), 0);
    EXPECT_GT(vm.GetHeight(), 0);
    EXPECT_GT(vm.GetDuration(), 0.0);
    EXPECT_NE(vm.GetSharedFence(), nullptr);

    vm.Play();

    // Consume frames until a valid one arrives (10 s timeout).
    const VideoFrame* frame = nullptr;
    for (int i = 0; i < 600; ++i) {
        frame = vm.Tick(0.0);
        if (frame && frame->valid) break;
        Sleep(16);
    }
    ASSERT_NE(frame, nullptr) << "no frame published within timeout";

    EXPECT_TRUE(frame->valid);
    EXPECT_NE(frame->sharedHandle, static_cast<HANDLE>(nullptr));
    EXPECT_GT(frame->fenceValue, 0ULL);
    EXPECT_EQ(static_cast<UINT>(frame->width), static_cast<UINT>(vm.GetWidth()));
    EXPECT_EQ(static_cast<UINT>(frame->height), static_cast<UINT>(vm.GetHeight()));

    // The D3D12 side must be able to open the pool slot's NT handle.
    Microsoft::WRL::ComPtr<ID3D12Resource> opened;
    const HRESULT hr = d3d12.Get()->OpenSharedHandle(
        frame->sharedHandle, IID_PPV_ARGS(&opened));
    EXPECT_TRUE(SUCCEEDED(hr)) << "OpenSharedHandle failed (0x" << std::hex << hr
                               << std::dec << ")";
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = opened->GetDesc();
        EXPECT_EQ(desc.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        EXPECT_EQ(desc.Format, DXGI_FORMAT_B8G8R8A8_UNORM);
        // The pool texture is created from the DECODED surface (16-aligned
        // coded dims), while GetWidth() reports the container/cropped dims;
        // the texture is therefore >= the logical frame.
        EXPECT_GE(desc.Width, static_cast<UINT64>(vm.GetWidth()));
        EXPECT_GE(desc.Height, static_cast<UINT64>(vm.GetHeight()));
    }

    // Slot fence must be monotonic and nonzero after the first blt.
    const UINT64 slotFence = vm.GetSlotFence(frame->slot);
    EXPECT_GT(slotFence, 0ULL);

    // A second frame must arrive (pipeline keeps producing).
    const VideoFrame* second = nullptr;
    for (int i = 0; i < 600; ++i) {
        second = vm.Tick(0.0);
        if (second && second->frameIndex > frame->frameIndex) break;
        Sleep(16);
    }
    ASSERT_NE(second, nullptr) << "no second frame published within timeout";
    EXPECT_GT(second->fenceValue, frame->fenceValue);

    const auto tel = vm.GetTelemetry();
    EXPECT_GE(tel.framesDecoded, 2ULL);

    vm.Stop();
    vm.Close();
    vm.Shutdown();
}

// In-repo long fixture (60 s — no loop wrap interferes with the Stop
// measurement); override via REX_TEST_VIDEO_LONG.
std::string FindLongTestVideo() {
    if (const char* env = std::getenv("REX_TEST_VIDEO_LONG")) {
        if (*env && std::filesystem::exists(env)) return env;
    }
    const std::string repoFixture =
        "D:/REX_doc/bench-media/cadence_1080p_60s.mp4";
    if (std::filesystem::exists(repoFixture)) return repoFixture;
    return std::string();
}

TEST(StopTimelineTest, StopRestartsPlaybackFromZero) {
    // P1-6 / R8: Stop must reset the whole A/V timeline (decoder seek 0 +
    // audio 0 + scheduler clear + position 0). Before the fix the decoder
    // kept its PTS, so Stop -> Play resumed video mid-stream while audio
    // restarted at 0.
    const std::string path = FindLongTestVideo();
    if (path.empty()) {
        GTEST_SKIP() << "no long test video (set REX_TEST_VIDEO_LONG)";
    }

    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable";
    }

    WallpaperConfig config;
    config.enableHardwareAcceleration = false;  // deterministic SW path

    VideoManager vm;
    if (vm.Initialize(config, d3d12.Get()).IsError()) {
        GTEST_SKIP() << "VideoManager init unavailable";
    }
    ASSERT_TRUE(vm.OpenVideo(path).IsSuccess());

    vm.Play();
    // Play ~2 s so a non-reset decoder would be clearly mid-stream.
    const VideoFrame* pre = nullptr;
    for (int i = 0; i < 600 && vm.GetPosition() < 2.0; ++i) {
        const VideoFrame* f = vm.Tick(0.0);
        if (f && f->valid) pre = f;
        Sleep(16);
    }
    ASSERT_NE(pre, nullptr) << "no frames decoded before Stop";
    EXPECT_GE(vm.GetPosition(), 1.5);

    vm.Stop();
    EXPECT_EQ(vm.GetPosition(), 0.0);

    vm.Play();
    // First valid frame after Stop -> Play must come from ~0, not from
    // where the decoder was (~2 s). Margin 1.0 s separates reset (<0.5
    // typical) from resume (>1.5).
    const VideoFrame* post = nullptr;
    for (int i = 0; i < 600; ++i) {
        const VideoFrame* f = vm.Tick(0.0);
        if (f && f->valid) {
            post = f;
            break;
        }
        Sleep(16);
    }
    ASSERT_NE(post, nullptr) << "no frames decoded after Stop -> Play";
    EXPECT_LT(post->pts, 1.0) << "decoder was not rewound by Stop";

    vm.Stop();
    vm.Close();
    vm.Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Per-stage telemetry contract (Phase 1)
// ═══════════════════════════════════════════════════════════════════════════
//
// The §18 question (is the ~33 fps ceiling in decode, convert, scheduler
// or present?) needs separate counters per stage. These tests pin the
// contract: the fields exist, default to zero, and displayHz reports the
// real monitor rate.

TEST(StageTelemetryTest, PlaybackTelemetryHasStageFieldsDefaultZero) {
    VideoPlaybackTelemetry t;
    EXPECT_DOUBLE_EQ(t.sourceFps, 0.0);
    EXPECT_DOUBLE_EQ(t.convertFps, 0.0);
    EXPECT_EQ(t.schedulerDrops, 0ULL);
    EXPECT_EQ(t.loopWraps, 0ULL);
    EXPECT_DOUBLE_EQ(t.playbackSpeed, 1.0);
}

TEST(StageTelemetryTest, EngineTelemetryHasStageFieldsDefaultZero) {
    EngineTelemetry t;
    EXPECT_DOUBLE_EQ(t.sourceFps, 0.0);
    EXPECT_DOUBLE_EQ(t.convertFps, 0.0);
    EXPECT_EQ(t.schedulerDrops, 0ULL);
    EXPECT_EQ(t.loopWraps, 0ULL);
    EXPECT_DOUBLE_EQ(t.presentFps, 0.0);
    EXPECT_EQ(t.repeatedFrames, 0ULL);
    EXPECT_EQ(t.queueDepth, 0ULL);
    EXPECT_EQ(t.displayHz, 0);
    EXPECT_DOUBLE_EQ(t.effectiveVideoFps, 0.0);
    EXPECT_DOUBLE_EQ(t.playbackSpeed, 1.0);
}

TEST(StageTelemetryTest, PrimaryMonitorReportsRefreshRate) {
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    bool sawMonitor = false;
    for (const auto& m : engine->EnumerateMonitors()) {
        sawMonitor = true;
        if (m.isPrimary) {
            EXPECT_GT(m.refreshHz, 0) << "primary monitor has no refresh rate";
        }
    }
    EXPECT_TRUE(sawMonitor) << "no monitors enumerated";
}

} // namespace
} // namespace rex::wallpaper
