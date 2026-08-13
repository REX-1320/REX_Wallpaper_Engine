#include "wallpaper/video/VideoManager.hpp"

#include "graphics/renderer/Device.hpp"

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

} // namespace
} // namespace rex::wallpaper
