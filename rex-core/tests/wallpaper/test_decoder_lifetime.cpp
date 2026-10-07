// ═══════════════════════════════════════════════════════════════════════════
// P1-8 regression tests: decoder lifetime vs concurrent access (R15)
// ═══════════════════════════════════════════════════════════════════════════
//
// VideoPlaybackController::Seek/SelectBestStream/GetSlotHandle dereferenced
// the decoder unique_ptr under only m_decodeMutex (or no lock at all) while
// Close reset it under m_threadMutex — concurrent Seek+Close (or a
// per-frame GetSlotHandle racing shutdown) is a use-after-free. The fix is
// one ownership mechanism: shared_ptr snapshots for every access, a single
// mutex for mutation, atomic decoder flags.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "wallpaper/playback/VideoPlaybackController.hpp"
#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/video/VideoManager.hpp"
#include "graphics/renderer/Device.hpp"

using namespace rex::wallpaper;

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// Decoder interrupt (P1-9 / R14): Close() must abort a blocked av_read_frame
// via AVIOInterruptCB. The callback reflects a stop flag: fresh decoders
// never interrupt; RequestStop()/Close() raise it; Open() clears it.
// ═══════════════════════════════════════════════════════════════════════════

TEST(DecoderInterruptTest, FreshDecoderDoesNotRequestInterrupt) {
    FfmpegVideoDecoder decoder;
    EXPECT_EQ(FfmpegVideoDecoder::InterruptCallback(&decoder), 0);
}

TEST(DecoderInterruptTest, RequestStopRaisesInterrupt) {
    FfmpegVideoDecoder decoder;
    decoder.RequestStop();
    EXPECT_NE(FfmpegVideoDecoder::InterruptCallback(&decoder), 0);
}

TEST(DecoderInterruptTest, CloseRequestsStop) {
    FfmpegVideoDecoder decoder;
    decoder.Close();  // null-safe without Open
    EXPECT_NE(FfmpegVideoDecoder::InterruptCallback(&decoder), 0);
}

// Every accessor on a never-opened controller must be null-safe (pins the
// guards through the shared_ptr refactor — dropping one segfaults here).

TEST(DecoderLifetimeTest, ClosedControllerAccessorsAreNullSafe) {
    VideoPlaybackController controller;
    EXPECT_FALSE(controller.IsOpen());
    EXPECT_EQ(controller.GetSlotHandle(0), nullptr);
    EXPECT_EQ(controller.GetSlotFence(0), 0u);
    EXPECT_TRUE(controller.Seek(1.0).IsError());
    EXPECT_NO_THROW(controller.Stop());
    const VideoPlaybackTelemetry tel = controller.GetTelemetry();
    EXPECT_EQ(tel.framesDecoded, 0u);
}

std::string FindLifetimeFixture() {
    if (const char* env = std::getenv("REX_TEST_VIDEO_SDR")) {
        if (*env && std::filesystem::exists(env)) return env;
    }
    const std::string repoFixture =
        "D:/REX_doc/bench-media/mpeg4_320x240.mp4";
    if (std::filesystem::exists(repoFixture)) return repoFixture;
    return std::string();
}

// Exercises the R15 race window: Seek/GetSlotHandle/GetTelemetry hammering
// from worker threads while the main thread Stops/Closes (and reopens).
// Pre-fix this is a unique_ptr use-after-free (may crash); post-fix every
// access snapshots null-or-live and the test completes deterministically.

TEST(DecoderLifetimeTest, ConcurrentSeekCloseAndReadCompletes) {
    const std::string path = FindLifetimeFixture();
    if (path.empty()) {
        GTEST_SKIP() << "no fixture video (set REX_TEST_VIDEO_SDR)";
    }

    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        GTEST_SKIP() << "D3D12 device unavailable";
    }

    WallpaperConfig config;
    config.enableHardwareAcceleration = false;

    VideoManager vm;
    if (vm.Initialize(config, d3d12.Get()).IsError()) {
        GTEST_SKIP() << "VideoManager init unavailable";
    }

    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(vm.OpenVideo(path).IsSuccess()) << "round " << round;
        vm.Play();

        std::atomic<bool> hammering{true};
        std::vector<std::thread> workers;
        for (int w = 0; w < 2; ++w) {
            workers.emplace_back([&, w] {
                for (int i = 0; i < 200 && hammering.load(); ++i) {
                    // Reader: per-frame render-thread path.
                    (void)vm.GetSlotHandle(0);
                    (void)vm.GetSlotFence(0);
                    (void)vm.GetTelemetry();
                    // Writer: command-thread path (mutates decoder state).
                    if ((i + w) % 2 == 0) {
                        (void)vm.Seek((i % 4) * 0.25);
                    }
                }
            });
        }
        // Close + reopen while workers hammer the old controller: the
        // exact Seek+Close / GetSlotHandle+reset interleave from R15.
        vm.Stop();
        vm.Close();
        hammering.store(false);
        for (auto& t : workers) t.join();
    }

    vm.Shutdown();
}

}  // namespace
