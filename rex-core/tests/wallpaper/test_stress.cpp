#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <filesystem>
#include <string>

using namespace rex::wallpaper;

namespace {

constexpr const char* kSampleImage =
    "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";

bool HasSampleImage() {
    return std::filesystem::exists(kSampleImage);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Rapid scene switching stress
// ═══════════════════════════════════════════════════════════════════════════

TEST(StressTest, RapidSceneSwitching) {
    if (!HasSampleImage()) {
        GTEST_SKIP() << "sample image not present";
    }

    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    // Alternate static scenes (real file) with video opens against a
    // nonexistent path. Missing files must FAIL LOUDLY under fail-open
    // hardening (not succeed): assert the error, then prove the engine
    // recovers and accepts the next LoadScene. That error path is the
    // stress — rapid switching must never wedge the state machine.
    for (int i = 0; i < 50; ++i) {
        SceneInfo scene;
        scene.id = "stress_" + std::to_string(i);
        if (i % 2 == 0) {
            scene.type = "static";
            scene.sourcePath = kSampleImage;
            auto loadResult = engine->LoadScene(scene);
            EXPECT_TRUE(loadResult.IsSuccess()) << "LoadScene(" << i << ") failed: "
                                                << loadResult.Error().message;
        } else {
            scene.type = "video";
            scene.sourcePath = "Z:\\missing\\clip.mp4";
            auto loadResult = engine->LoadScene(scene);
            EXPECT_TRUE(loadResult.IsError()) << "missing file loaded silently ("
                                              << i << ")";
        }
        auto unloadResult = engine->UnloadScene();
        EXPECT_TRUE(unloadResult.IsSuccess()) << "UnloadScene(" << i << ") failed: "
                                              << unloadResult.Error().message;
    }

    engine->Shutdown();
}

TEST(StressTest, RapidPlayPauseStop) {
    if (!HasSampleImage()) {
        GTEST_SKIP() << "sample image not present";
    }

    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    SceneInfo scene;
    scene.id = "play_pause";
    scene.sourcePath = kSampleImage;
    scene.type = "static";
    ASSERT_TRUE(engine->LoadScene(scene).IsSuccess());

    // Hammer the state machine: every cycle starts from Stop, so each
    // Play/Pause pair must succeed.
    for (int i = 0; i < 200; ++i) {
        EXPECT_TRUE(engine->Play().IsSuccess()) << "Play(" << i << ")";
        EXPECT_TRUE(engine->Pause().IsSuccess()) << "Pause(" << i << ")";
        EXPECT_TRUE(engine->Stop().IsSuccess()) << "Stop(" << i << ")";
        EXPECT_FALSE(engine->IsPlaying());
    }

    engine->Shutdown();
}
