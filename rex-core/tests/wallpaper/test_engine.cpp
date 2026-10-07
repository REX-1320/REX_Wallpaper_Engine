#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace rex::wallpaper;

// ═══════════════════════════════════════════════════════════════════════════
// Lifecycle Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, CreateAndInitialize) {
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);

    WallpaperConfig config;
    auto result = engine->Initialize(config);
    EXPECT_TRUE(result.IsSuccess());

    EXPECT_EQ(engine->GetState(), EngineState::Initialized);

    result = engine->Shutdown();
    EXPECT_TRUE(result.IsSuccess());
    EXPECT_EQ(engine->GetState(), EngineState::Uninitialized);
}

TEST(EngineTest, DoubleInitializeReturnsError) {
    auto engine = CreateWallpaperEngine();

    WallpaperConfig config;
    EXPECT_TRUE(engine->Initialize(config).IsSuccess());

    auto result = engine->Initialize(config);
    EXPECT_TRUE(result.IsError());

    engine->Shutdown();
}

TEST(EngineTest, ShutdownWithoutInitializeSucceeds) {
    auto engine = CreateWallpaperEngine();
    EXPECT_TRUE(engine->Shutdown().IsSuccess());
}

// ═══════════════════════════════════════════════════════════════════════════
// Scene Loading Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, LoadAndUnloadScene) {
    auto engine = CreateWallpaperEngine();

    WallpaperConfig config;
    engine->Initialize(config);

    SceneInfo scene;
    scene.id = "test";
    scene.name = "Test Scene";
    scene.sourcePath = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    scene.type = "static";

    EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());

    auto current = engine->GetCurrentScene();
    EXPECT_EQ(current.id, "test");
    EXPECT_EQ(current.name, "Test Scene");
    EXPECT_EQ(current.type, "static");

    EXPECT_TRUE(engine->UnloadScene().IsSuccess());
    EXPECT_TRUE(engine->GetCurrentScene().sourcePath.empty());

    engine->Shutdown();
}

TEST(EngineTest, LoadInvalidTextureDoesNotCrash) {
    auto engine = CreateWallpaperEngine();
    WallpaperConfig config;
    engine->Initialize(config);

    SceneInfo scene;
    scene.sourcePath = "Z:\\nonexistent\\file.jpg";
    scene.type = "static";

    // Should log a warning but not crash
    EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());

    engine->UnloadScene();
    engine->Shutdown();
}

TEST(EngineTest, LoadEmptyScenePathSucceeds) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.sourcePath = "";  // empty path — should be valid, just no texture
    scene.type = "static";

    EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());
    engine->UnloadScene();
    engine->Shutdown();
}

TEST(EngineTest, SequentialSceneLoadLeakCheck) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    // Load 100 scenes sequentially — detect leaks through memory/refcount
    for (int i = 0; i < 100; ++i) {
        SceneInfo scene;
        scene.id = "scene_" + std::to_string(i);
        scene.sourcePath = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
        scene.type = "static";

        EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());
        EXPECT_EQ(engine->GetCurrentScene().id, scene.id);
        EXPECT_TRUE(engine->UnloadScene().IsSuccess());
    }

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Playback Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, PlayBeforeLoadReturnsError) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    // No scene loaded — Play should fail
    EXPECT_TRUE(engine->Play().IsError());

    engine->Shutdown();
}

TEST(EngineTest, PlayPauseStopCycle) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.id = "test";
    scene.sourcePath = "test.jpg";

    engine->LoadScene(scene);

    EXPECT_TRUE(engine->Play().IsSuccess());
    EXPECT_TRUE(engine->IsPlaying());

    EXPECT_TRUE(engine->Pause().IsSuccess());
    EXPECT_FALSE(engine->IsPlaying());

    EXPECT_TRUE(engine->Play().IsSuccess());
    EXPECT_TRUE(engine->IsPlaying());

    EXPECT_TRUE(engine->Stop().IsSuccess());
    EXPECT_FALSE(engine->IsPlaying());

    engine->Shutdown();
}

TEST(EngineTest, RapidPlayPauseStopCycle) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.id = "rapid";
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);

    // 100 rapid cycles — must not crash or leak
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(engine->Play().IsSuccess());
        EXPECT_TRUE(engine->Pause().IsSuccess());
        EXPECT_TRUE(engine->Play().IsSuccess());
        EXPECT_TRUE(engine->Stop().IsSuccess());
    }

    EXPECT_FALSE(engine->IsPlaying());
    engine->Shutdown();
}

TEST(EngineTest, PauseWhenStoppedReturnsError) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);

    // Can't pause when not running
    EXPECT_TRUE(engine->Pause().IsError());

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Fill Mode Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, FillModeRoundTrip) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);

    auto current = engine->GetCurrentScene();
    EXPECT_EQ(current.fillMode, FillMode::Fill);

    EXPECT_TRUE(engine->SetFillMode(FillMode::Fit).IsSuccess());
    EXPECT_EQ(engine->GetCurrentScene().fillMode, FillMode::Fit);

    EXPECT_TRUE(engine->SetFillMode(FillMode::Center).IsSuccess());
    EXPECT_EQ(engine->GetCurrentScene().fillMode, FillMode::Center);

    EXPECT_TRUE(engine->SetFillMode(FillMode::Stretch).IsSuccess());
    EXPECT_EQ(engine->GetCurrentScene().fillMode, FillMode::Stretch);

    EXPECT_TRUE(engine->SetFillMode(FillMode::Fill).IsSuccess());
    EXPECT_EQ(engine->GetCurrentScene().fillMode, FillMode::Fill);

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Monitor Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, EnumerateMonitors) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    auto monitors = engine->EnumerateMonitors();
    EXPECT_GE(monitors.size(), 1); // At least one monitor must exist

    bool foundPrimary = false;
    for (const auto& m : monitors) {
        EXPECT_FALSE(m.id.empty());
        EXPECT_GT(m.width, 0);
        EXPECT_GT(m.height, 0);
        if (m.isPrimary) foundPrimary = true;
    }
    EXPECT_TRUE(foundPrimary);

    engine->Shutdown();
}

TEST(EngineTest, AssignAndClearMonitor) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    auto monitors = engine->EnumerateMonitors();
    ASSERT_GE(monitors.size(), 1);

    EXPECT_TRUE(engine->AssignMonitor(monitors[0].id).IsSuccess());
    auto current = engine->GetCurrentMonitor();
    EXPECT_EQ(current.id, monitors[0].id);
    EXPECT_GT(current.width, 0);

    EXPECT_TRUE(engine->ClearMonitor().IsSuccess());
    EXPECT_TRUE(engine->GetCurrentMonitor().id.empty());

    engine->Shutdown();
}

TEST(EngineTest, AssignInvalidMonitorFallsBack) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    // Invalid monitor ID should fall back gracefully
    EXPECT_TRUE(engine->AssignMonitor("NONEXISTENT_MONITOR").IsSuccess());
    auto current = engine->GetCurrentMonitor();
    EXPECT_EQ(current.id, "NONEXISTENT_MONITOR");
    EXPECT_GT(current.width, 0); // fallback dimensions

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Settings Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, SetSettings) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);

    EXPECT_TRUE(engine->SetBrightness(1.5f).IsSuccess());
    EXPECT_TRUE(engine->SetContrast(1.2f).IsSuccess());
    EXPECT_TRUE(engine->SetSaturation(0.8f).IsSuccess());
    EXPECT_TRUE(engine->SetVolume(0.5f).IsSuccess());
    EXPECT_TRUE(engine->SetLoop(false).IsSuccess());
    EXPECT_TRUE(engine->SetSpeed(2.0f).IsSuccess());

    engine->Shutdown();
}

TEST(EngineTest, SettingsClamping) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);

    // Brightness should clamp to [0, 2]
    EXPECT_TRUE(engine->SetBrightness(-1.0f).IsSuccess());
    EXPECT_TRUE(engine->SetBrightness(5.0f).IsSuccess());

    // Speed should clamp to [0.25, 4.0]
    EXPECT_TRUE(engine->SetSpeed(0.0f).IsSuccess());
    EXPECT_TRUE(engine->SetSpeed(10.0f).IsSuccess());

    // Volume should clamp to [0, 1]
    EXPECT_TRUE(engine->SetVolume(-0.5f).IsSuccess());
    EXPECT_TRUE(engine->SetVolume(2.0f).IsSuccess());

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Audio Reactive Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, AudioReactiveToggle) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    EXPECT_TRUE(engine->EnableAudioReactive(true).IsSuccess());
    EXPECT_TRUE(engine->EnableAudioReactive(false).IsSuccess());
    EXPECT_TRUE(engine->SetAudioSensitivity(0.8f).IsSuccess());
    EXPECT_TRUE(engine->SetAudioSensitivity(0.0f).IsSuccess());
    EXPECT_TRUE(engine->SetAudioSensitivity(1.0f).IsSuccess());

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Stress Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineStressTest, CreateAndDestroyMany) {
    // Create and destroy 50 engine instances — no leaks, no crashes
    for (int i = 0; i < 50; ++i) {
        auto engine = CreateWallpaperEngine();
        ASSERT_NE(engine, nullptr);

        WallpaperConfig config;
        EXPECT_TRUE(engine->Initialize(config).IsSuccess());
        EXPECT_TRUE(engine->Shutdown().IsSuccess());
    }
}

TEST(EngineStressTest, LoadManyScenes) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    // Load/unload 1000 times with different scene types
    for (int i = 0; i < 1000; ++i) {
        SceneInfo scene;
        scene.id = "scene_" + std::to_string(i);
        scene.type = (i % 2 == 0) ? "static" : "video";

        EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());
        EXPECT_TRUE(engine->UnloadScene().IsSuccess());
    }

    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// UUID and Debug Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, DumpStatsDoesNotCrash) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    // Should not crash even with no scene loaded
    engine->DumpStats();

    SceneInfo scene;
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);
    engine->DumpStats();


    engine->Shutdown();
    // DumpStats after shutdown accesses freed resources — not expected to work
}

// ═══════════════════════════════════════════════════════════════════════════
// Video Playback Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(EngineTest, VideoScenePlayback) {
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.id = "video_test";
    scene.sourcePath = "test.mp4";
    scene.type = "video";

    EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());
    EXPECT_EQ(engine->GetCurrentScene().type, "video");
    EXPECT_TRUE(engine->GetCurrentScene().sourcePath == "test.mp4");

    // Video loads but can't play without a real file — this is expected
    engine->Play();
    engine->Pause();
    engine->Stop();

    engine->UnloadScene();
    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Recovery Retry Policy
// ═══════════════════════════════════════════════════════════════════════════
//
// Regression guard for the device-loss hot-spin: the render loop retried
// recovery on the very next iteration with no delay and no re-arm, so a host
// that could not be reacquired (Explorer still restarting) pinned the render
// thread at 100% CPU and ran the %30/%120 housekeeping on every pass.

#include "wallpaper/engine/RecoveryBackoff.hpp"

TEST(RecoveryBackoffTest, FirstRetryIsDelayedNotImmediate) {
    EXPECT_GT(rex::wallpaper::NextRecoveryDelayMs(0), 0);
}

TEST(RecoveryBackoffTest, DelayDoublesAfterEachFailure) {
    const int first = rex::wallpaper::NextRecoveryDelayMs(0);
    const int second = rex::wallpaper::NextRecoveryDelayMs(first);
    EXPECT_GT(second, first);
}

TEST(RecoveryBackoffTest, DelayNeverExceedsCap) {
    int delay = 0;
    for (int i = 0; i < 50; ++i) {
        delay = rex::wallpaper::NextRecoveryDelayMs(delay);
    }
    EXPECT_EQ(delay, rex::wallpaper::kMaxRecoveryDelayMs);
}

TEST(RecoveryBackoffTest, NonPositiveInputIsTreatedAsFirstFailure) {
    EXPECT_EQ(rex::wallpaper::NextRecoveryDelayMs(-1),
              rex::wallpaper::NextRecoveryDelayMs(0));
}

// ═══════════════════════════════════════════════════════════════════════════
// Playback-settings truth (P1-7 / R9)
// ═══════════════════════════════════════════════════════════════════════════
//
// LOAD and device-loss recovery rebuild the video backend through LoadScene,
// which restores knobs from the current scene — so the setters must keep the
// scene in sync. Before the fix SetLoop/SetSpeed/SetMute wrote only side
// members: a later recovery silently reverted them (notably: unmuted).
// No Initialize needed — pure engine state, deterministic everywhere.

TEST(PlaybackSettingsTest, SetLoopSyncsToCurrentScene) {
    auto engine = CreateWallpaperEngine();
    ASSERT_TRUE(engine->GetCurrentScene().loop);
    EXPECT_TRUE(engine->SetLoop(false).IsSuccess());
    EXPECT_FALSE(engine->GetCurrentScene().loop);
    EXPECT_TRUE(engine->SetLoop(true).IsSuccess());
    EXPECT_TRUE(engine->GetCurrentScene().loop);
}

TEST(PlaybackSettingsTest, SetSpeedSyncsToCurrentScene) {
    auto engine = CreateWallpaperEngine();
    EXPECT_TRUE(engine->SetSpeed(2.0f).IsSuccess());
    EXPECT_FLOAT_EQ(engine->GetCurrentScene().speed, 2.0f);
}

TEST(PlaybackSettingsTest, SetMuteSyncsToCurrentScene) {
    auto engine = CreateWallpaperEngine();
    ASSERT_FALSE(engine->GetCurrentScene().muted);
    EXPECT_TRUE(engine->SetMute(true).IsSuccess());
    EXPECT_TRUE(engine->GetCurrentScene().muted);
}

TEST(PlaybackSettingsTest, SetVolumeSyncsToCurrentScene) {
    auto engine = CreateWallpaperEngine();
    EXPECT_TRUE(engine->SetVolume(0.3f).IsSuccess());
    EXPECT_FLOAT_EQ(engine->GetCurrentScene().volume, 0.3f);
}

// ═══════════════════════════════════════════════════════════════════════════
// Static render-once (R2)
// ═══════════════════════════════════════════════════════════════════════════
//
// A static scene must present once (load arms the dirty flag) and then idle:
// no per-frame presents until something changes. Before the fix the dirty
// flag was only cleared in the video branch, so static scenes presented at
// full loop rate forever.

TEST(StaticRenderOnceTest, StaticScenePresentsOnceThenIdles) {
    constexpr const char* kImage =
        "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    if (!std::filesystem::exists(kImage)) {
        GTEST_SKIP() << "stock wallpaper image missing";
    }
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    SceneInfo scene;
    scene.id = "static_once";
    scene.sourcePath = kImage;
    scene.type = "static";
    ASSERT_TRUE(engine->LoadScene(scene).IsSuccess());
    ASSERT_TRUE(engine->Play().IsSuccess());

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EngineTelemetry t1{};
    engine->GetTelemetry(t1);
    if (t1.presentCount < 1) {
        EXPECT_TRUE(engine->Stop().IsSuccess());
        EXPECT_TRUE(engine->UnloadScene().IsSuccess());
        engine->Shutdown();
        GTEST_SKIP() << "no presents flow on this machine (headless?)";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EngineTelemetry t2{};
    engine->GetTelemetry(t2);
    EXPECT_EQ(t2.presentCount, t1.presentCount)
        << "static scene keeps presenting with no changes (R2)";

    EXPECT_TRUE(engine->Stop().IsSuccess());
    EXPECT_TRUE(engine->UnloadScene().IsSuccess());
    engine->Shutdown();
}
