#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace rex::wallpaper;

// ═══════════════════════════════════════════════════════════════════════════
// Long-running soak skeleton
// ═══════════════════════════════════════════════════════════════════════════
//
// Disabled by default. Run with:
//   rex-wallpaper-tests --gtest_filter=SoakTest.* \
//                       --gtest_also_run_disabled_tests
//
// Set REX_TEST_VIDEO to a local clip when video playback should be part of
// the soak; without it the test runs with the bundled sample image.

TEST(SoakTest, DISABLED_LongRunningPlayback) {
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    SceneInfo scene;
    scene.id = "soak";
    scene.loop = true;

    std::string videoPath;
    if (const char* env = std::getenv("REX_TEST_VIDEO")) {
        if (std::filesystem::exists(env)) videoPath = env;
    }
    if (!videoPath.empty()) {
        scene.type = "video";
        scene.sourcePath = videoPath;
    } else {
        scene.type = "static";
        scene.sourcePath = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    }
    ASSERT_TRUE(engine->LoadScene(scene).IsSuccess());
    ASSERT_TRUE(engine->Play().IsSuccess());

    // 30 minutes of playback, one sample per second.
    for (int i = 0; i < 1800; ++i) {
        Sleep(1000);
        EXPECT_TRUE(engine->IsPlaying()) << "playback stopped at second " << i;
        if (HasFatalFailure()) break;
    }

    engine->Stop();
    engine->Shutdown();
}
