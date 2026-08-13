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

    // Alternate static/video scenes. Video opens against a nonexistent
    // path on purpose — the error path is part of the stress (the engine
    // must recover and accept the next LoadScene).
    for (int i = 0; i < 50; ++i) {
        SceneInfo scene;
        scene.id = "stress_" + std::to_string(i);
        scene.type = (i % 2 == 0) ? "static" : "video";
        scene.sourcePath = (i % 2 == 0) ? kSampleImage : "Z:\\missing\\clip.mp4";

        auto loadResult = engine->LoadScene(scene);
        EXPECT_TRUE(loadResult.IsSuccess()) << "LoadScene(" << i << ") failed: "
                                            << loadResult.Error().message;
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
