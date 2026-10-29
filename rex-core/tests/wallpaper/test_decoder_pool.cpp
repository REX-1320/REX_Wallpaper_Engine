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
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/video/VideoManager.hpp"
#include "wallpaper/engine/ResourcePolicy.hpp"
#include "wallpaper/playback/FrameScheduler.hpp"
#include "graphics/renderer/Device.hpp"

using rex::wallpaper::FfmpegVideoDecoder;

// Slot-count mirror pin: the envelope's footprint convention must track the
// real rings. Update ResourcePolicy.hpp together with either constant.
static_assert(FfmpegVideoDecoder::kPoolSize ==
                  rex::wallpaper::kDecodePoolSlots,
              "decode pool size drifted from the resource envelope");
static_assert(rex::wallpaper::FrameScheduler::kNumSlots ==
                  rex::wallpaper::kSchedulerRingSlots,
              "scheduler ring size drifted from the resource envelope");

// ─── Media resource envelope (P1) ─────────────────────────────────────────
// Pool footprint = w*h*4*(8+8); 4K ≈ 530 MB, 8K ≈ 2.1 GB.

TEST(MediaBudgetTest, FourKInBigBudgetLoads) {
    const auto r = rex::wallpaper::AssessMediaBudget(
        3840, 2160, 4ULL * 1024 * 1024 * 1024);
    EXPECT_FALSE(r.overBudget);
    EXPECT_FALSE(r.unknownBudget);
    EXPECT_EQ(r.poolBytes, 3840ULL * 2160 * 4 * 16);
}

TEST(MediaBudgetTest, EightKInSmallBudgetRefuses) {
    const auto r = rex::wallpaper::AssessMediaBudget(
        7680, 4320, 1ULL * 1024 * 1024 * 1024);
    EXPECT_TRUE(r.overBudget);
    EXPECT_FALSE(r.unknownBudget);
}

TEST(MediaBudgetTest, EightKInBigBudgetLoadsNoArbitraryCap) {
    const auto r = rex::wallpaper::AssessMediaBudget(
        7680, 4320, 8ULL * 1024 * 1024 * 1024);
    EXPECT_FALSE(r.overBudget);
}

TEST(MediaBudgetTest, ExactBoundaryLoads) {
    const uint64_t footprint =
        rex::wallpaper::PoolFootprintBytes(1920, 1080);
    const auto r = rex::wallpaper::AssessMediaBudget(1920, 1080, footprint);
    EXPECT_FALSE(r.overBudget);
}

TEST(MediaBudgetTest, InvalidDimensionsRefuse) {
    EXPECT_TRUE(rex::wallpaper::AssessMediaBudget(0, 1080, 1ULL << 40).overBudget);
    EXPECT_TRUE(rex::wallpaper::AssessMediaBudget(1920, -1, 1ULL << 40).overBudget);
    EXPECT_TRUE(rex::wallpaper::AssessMediaBudget(20000, 1080, 1ULL << 40).overBudget);
}

TEST(MediaBudgetTest, UnknownBudgetFailsOpenWithWarning) {
    const auto r = rex::wallpaper::AssessMediaBudget(3840, 2160, 0);
    EXPECT_FALSE(r.overBudget);
    EXPECT_TRUE(r.unknownBudget);
}

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

// The pool must mirror the decoder SURFACE, not the SDR/HDR flag: 10-bit
// content (Main10) decodes to P010 surfaces whether or not its transfer is
// PQ/HLG, and a P010 surface copied into an NV12 pool is a silent no-op
// (CopySubresourceRegion drops format mismatches) — every frame then renders
// as garbage. The flag only selects the shader math via the frame's colour
// info; the pool ↔ surface contract always holds.
TEST(DecoderPoolFormatTest, HardwareSdrWithP010SurfaceKeepsP010Pool) {
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, false,
                                                    DXGI_FORMAT_P010),
              DXGI_FORMAT_P010);
}

TEST(DecoderPoolFormatTest, HardwareHdrWithNv12SurfaceKeepsNv12Pool) {
    // Inverse direction: an 8-bit surface must never be copied into P010.
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, true,
                                                    DXGI_FORMAT_NV12),
              DXGI_FORMAT_NV12);
}

TEST(DecoderPoolFormatTest, HardwareSdrWithNv12SurfaceStaysNv12) {
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, false,
                                                    DXGI_FORMAT_NV12),
              DXGI_FORMAT_NV12);
}

TEST(DecoderPoolFormatTest, HardwareHdrWithP010SurfaceStaysP010) {
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, true,
                                                    DXGI_FORMAT_P010),
              DXGI_FORMAT_P010);
}

TEST(DecoderPoolFormatTest, UnhandledSurfaceFallsBackToFlagChoice) {
    // Surface formats the copy path cannot express keep the historical
    // flag-based mapping (the copy path logs them).
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, false,
                                                    DXGI_FORMAT_YUY2),
              DXGI_FORMAT_NV12);
    EXPECT_EQ(FfmpegVideoDecoder::PoolTextureFormat(true, true,
                                                    DXGI_FORMAT_YUY2),
              DXGI_FORMAT_P010);
}

// ── Pool generation reuse (2026-10-28 VRAM growth) ────────────────────────
//
// Every format switch used to destroy the pool and build a fresh NT-handle
// generation. The D3D12 renderer has to import each new handle, and the AMD
// driver charges a permanent dedicated-VRAM footprint per imported generation
// (released only at process exit — measured 2026-10-28: +264 MB per SDR<->HDR
// switch, +2.1 GB after 16 switches). The fix is to hand back a generation the
// cache already built for the same geometry + format instead of rebuilding.
// These pin the lookup the cache is built on.
TEST(DecoderPoolCacheTest, FindsMatchingGenerationAndLruVictim) {
    using Key = FfmpegVideoDecoder::PoolGenerationKey;
    const Key keys[2] = {{3840, 2160, DXGI_FORMAT_NV12},
                         {3840, 2176, DXGI_FORMAT_P010}};

    EXPECT_EQ(FfmpegVideoDecoder::FindPoolGeneration(
                  keys, 2, Key{3840, 2160, DXGI_FORMAT_NV12}), 0u);
    EXPECT_EQ(FfmpegVideoDecoder::FindPoolGeneration(
                  keys, 2, Key{3840, 2176, DXGI_FORMAT_P010}), 1u);
    // A geometry the cache never built is a miss (== count).
    EXPECT_EQ(FfmpegVideoDecoder::FindPoolGeneration(
                  keys, 2, Key{1920, 1080, DXGI_FORMAT_NV12}), 2u);
    // Format is part of the key: same geometry, other format is a miss.
    EXPECT_EQ(FfmpegVideoDecoder::FindPoolGeneration(
                  keys, 2, Key{3840, 2160, DXGI_FORMAT_P010}), 2u);
    EXPECT_EQ(FfmpegVideoDecoder::FindPoolGeneration(keys, 0, Key{}), 0u);

    // LRU victim = smallest usage tick.
    const uint64_t ticks[3] = {7, 3, 5};
    EXPECT_EQ(FfmpegVideoDecoder::PickLruPoolGeneration(ticks, 3), 1u);
    EXPECT_EQ(FfmpegVideoDecoder::PickLruPoolGeneration(ticks, 1), 0u);
    EXPECT_EQ(FfmpegVideoDecoder::PickLruPoolGeneration(nullptr, 0), 0u);
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

// ── Audit 2026-10-17, §5.2 #4 (cross-listed §5.4 #3): SW path skipped the
//    pool-slot lease ────────────────────────────────────────────────────────
//
// The software conversion path rotated a blind cursor (m_poolNext) and never
// stamped m_slotFrameIndex, so it could overwrite a slot the renderer was
// still reading and left the lease bookkeeping stale for later consumers.
// It must lease through the same consumed-watermark allocator as the HW
// path. Deterministic trace with no consumer watermark and one stamp per
// decoded frame (strict-stalest rule, ties keep the lowest index):
//   f0 -> 0, f1 -> 0 (slot 0 was just stamped, still stalest), f2 -> 1,
//   f3 -> 2, ... f8 -> 7.
// The pre-fix blind rotation puts f1 in slot 1 — frame 1 discriminates.
TEST(SoftwareDecodePoolTest, SoftwarePathLeasesPoolSlots) {
    const std::string path = FindSdrTestVideo();
    if (path.empty()) {
        GTEST_SKIP() << "no SDR test video (set REX_TEST_VIDEO_SDR)";
    }

    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable";
    }

    // Same-adapter D3D11 device + immediate context (the VideoManager
    // recipe): the decoder needs the context; the device comes from it.
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(d3d12.Get()->GetAdapterLuid(),
                                          IID_PPV_ARGS(&adapter)))) {
        GTEST_SKIP() << "cannot query D3D12 adapter";
    }
    Microsoft::WRL::ComPtr<ID3D11Device> dev11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx11;
    const UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, createFlags, &fl, 1,
                                 D3D11_SDK_VERSION, &dev11, nullptr, &ctx11))) {
        GTEST_SKIP() << "D3D11 device creation failed";
    }

    // Shared fence (D3D12 creates, D3D11 opens): the SW path signals it
    // after every upload, exactly like the HW path.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    HANDLE fenceHandle = nullptr;
    if (FAILED(d3d12.Get()->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                        IID_PPV_ARGS(&fence12))) ||
        FAILED(d3d12.Get()->CreateSharedHandle(fence12.Get(), nullptr,
                                               GENERIC_ALL, nullptr,
                                               &fenceHandle))) {
        GTEST_SKIP() << "shared fence creation failed";
    }

    FfmpegVideoDecoder decoder;
    const auto openResult =
        decoder.Open(path, /*useHardware=*/false, ctx11.Get(), fenceHandle);
    ASSERT_TRUE(openResult.IsSuccess()) << openResult.Error().message;

    const UINT expected[9] = {0, 0, 1, 2, 3, 4, 5, 6, 7};
    for (UINT i = 0; i < 9; ++i) {
        rex::wallpaper::VideoFrame frame;
        ASSERT_TRUE(decoder.DecodeFrame(frame))
            << "fixture ended at frame " << i;
        ASSERT_TRUE(frame.valid);
        ASSERT_EQ(frame.frameIndex, i);
        EXPECT_EQ(frame.slot, expected[i])
            << "frame " << i << ": SW path left the slot lease "
               "(audit §5.2 #4)";
    }

    decoder.Close();
    ::CloseHandle(fenceHandle);
}
