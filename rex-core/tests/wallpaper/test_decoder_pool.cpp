// ═══════════════════════════════════════════════════════════════════════════
// P0-3 regression tests: decoder pool format follows the decode path
// ═══════════════════════════════════════════════════════════════════════════
//
// R3: EnsurePool built m_hdrContent ? P010 : NV12 while ConvertSoftware
// produces BGRA (SDR) / RGBA64 (HDR) bytes — every software-decoded frame
// was uploaded into a YUV pool (corrupt frames). R4: the fast path
// required m_processor (null on software decode), rebuilding the pool
// every frame. Desired: HW → P010/NV12 GPU pool, SW SDR → BGRA pool,
// SW HDR → RGBA16 pool, created one-time per resolution/format.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <windows.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/video/VideoManager.hpp"
#include "graphics/renderer/Device.hpp"

using rex::wallpaper::FfmpegVideoDecoder;

TEST(DecoderPoolFormatTest, HardwareSdrIsNv12) {
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, false),
              DXGI_FORMAT_NV12);
}

TEST(DecoderPoolFormatTest, HardwareHdrIsP010) {
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, true),
              DXGI_FORMAT_P010);
}

TEST(DecoderPoolFormatTest, SoftwareSdrIsBgra) {
    // BGRA bytes must land in a BGRA pool, never in an NV12 pool.
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(false, false),
              DXGI_FORMAT_B8G8R8A8_UNORM);
}

TEST(DecoderPoolFormatTest, SoftwareHdrIsRgba16) {
    // RGBA64 bytes must land in a 16-bit UNORM RGBA pool, never P010.
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(false, true),
              DXGI_FORMAT_R16G16B16A16_UNORM);
}

namespace {

// In-repo SDR fixture (deterministic); override via REX_TEST_VIDEO_SDR.
std::string FindSdrTestVideo() {
    if (const char* env = std::getenv("REX_TEST_VIDEO_SDR")) {
        if (*env && std::filesystem::exists(env)) return env;
    }
    const std::string repoFixture =
        "D:/REX_doc/bench-media/mpeg4_320x240.mp4";
    if (std::filesystem::exists(repoFixture)) return repoFixture;
    return std::string();
}

}  // namespace

TEST(SoftwareDecodePoolTest, BgraPoolServesValidRgbFrames) {
    const std::string path = FindSdrTestVideo();
    if (path.empty()) {
        GTEST_SKIP() << "no SDR test video (set REX_TEST_VIDEO_SDR)";
    }

    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable";
    }

    rex::wallpaper::WallpaperConfig config;
    config.enableHardwareAcceleration = false;  // force the SW path

    rex::wallpaper::VideoManager vm;
    if (vm.Initialize(config, d3d12.Get()).IsError()) {
        GTEST_SKIP() << "VideoManager init unavailable";
    }
    ASSERT_TRUE(vm.OpenVideo(path).IsSuccess());
    vm.Play();

    // Consume frames until a valid one arrives (10 s timeout).
    const rex::wallpaper::VideoFrame* frame = nullptr;
    for (int i = 0; i < 600; ++i) {
        frame = vm.Tick(0.0);
        if (frame && frame->valid) break;
        Sleep(16);
    }
    ASSERT_NE(frame, nullptr) << "no frame published within timeout";
    EXPECT_TRUE(frame->valid);
    EXPECT_NE(frame->sharedHandle, static_cast<HANDLE>(nullptr));
    EXPECT_GT(frame->fenceValue, 0ULL);
}
