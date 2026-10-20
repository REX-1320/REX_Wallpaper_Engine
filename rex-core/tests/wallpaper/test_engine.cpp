#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include "wallpaper/engine/PositionClock.hpp"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace rex::wallpaper;

namespace {

// Test-gap fixture (was: hardcoded "test.mp4" in CWD, failing by design
// under fail-open hardening). Resolution order: REX_TEST_VIDEO env,
// in-repo bench-media fixture, legacy CWD name; SKIP when absent so the
// suite stays honest instead of red-on-missing-file.
std::string FindEngineTestVideo() {
    if (const char* env = std::getenv("REX_TEST_VIDEO")) {
        if (*env && std::filesystem::exists(env)) return env;
    }
    const std::string repoFixture =
        "D:/REX_doc/bench-media/mpeg4_320x240.mp4";
    if (std::filesystem::exists(repoFixture)) return repoFixture;
    if (std::filesystem::exists("test.mp4")) return "test.mp4";
    return std::string();
}

}  // namespace

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

TEST(EngineTelemetryTest, TryGetTelemetrySucceedsWhenUncontended) {
    // Audit 5.5 #2: the hang watchdog must be able to sample progress
    // without blocking on the engine lock. Uncontended, the non-blocking
    // path must return a snapshot that agrees with GetTelemetry.
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);

    EngineTelemetry blocking{};
    engine->GetTelemetry(blocking);
    EngineTelemetry nonBlocking{};
    ASSERT_TRUE(engine->TryGetTelemetry(nonBlocking));
    EXPECT_EQ(nonBlocking.presentCount, blocking.presentCount);
}

TEST(EngineTest, StopWithoutInitializeRejected) {
    // Audit 2026-10-17, #8: Stop()/Play() on a torn-down engine used to
    // "succeed" (Stop stamped Stopped onto an uninitialized object — or
    // onto an object whose Shutdown ran concurrently).
    auto engine = CreateWallpaperEngine();
    EXPECT_TRUE(engine->Stop().IsError());
    EXPECT_TRUE(engine->Play().IsError());
    EXPECT_EQ(engine->GetState(), EngineState::Uninitialized);
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

TEST(EngineTest, LoadUnsupportedTypeFailsLoudly) {
    auto engine = CreateWallpaperEngine();

    WallpaperConfig config;
    engine->Initialize(config);

    // web/shader/scene/audio-file must never load as a dark silent scene.
    for (const char* bad : {"web", "shader", "scene", "audio-file", ""}) {
        SceneInfo scene;
        scene.id = "bad";
        scene.name = "Bad";
        scene.sourcePath = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
        scene.type = bad;
        auto result = engine->LoadScene(scene);
        EXPECT_TRUE(result.IsError()) << "type=" << bad;
    }

    // Rejected loads must not become current.
    EXPECT_TRUE(engine->GetCurrentScene().sourcePath.empty());

    engine->Shutdown();
}

TEST(EngineTest, LoadTileFillModeFailsLoudly) {
    // Tile has no D3D12 tiling implementation (ComputeFillViewport no-ops
    // it into a silent Stretch): LoadScene must refuse instead of
    // misrendering. OS wallpapers (Path A) still tile natively.
    auto engine = CreateWallpaperEngine();

    WallpaperConfig config;
    engine->Initialize(config);

    SceneInfo scene;
    scene.id = "tile";
    scene.name = "Tile";
    scene.sourcePath = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    scene.type = "static";
    scene.fillMode = FillMode::Tile;
    auto result = engine->LoadScene(scene);
    EXPECT_TRUE(result.IsError()) << "tile fillMode loaded silently";
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

TEST(EngineTest, PlayStopRejectedAfterShutdown) {
    auto engine = CreateWallpaperEngine();
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    SceneInfo scene;
    scene.id = "post-shutdown";
    scene.sourcePath = "test.jpg";
    engine->LoadScene(scene);

    ASSERT_TRUE(engine->Shutdown().IsSuccess());
    EXPECT_EQ(engine->GetState(), EngineState::Uninitialized);

    // Both commands must refuse rather than resurrect state onto dead
    // backends (audit 2026-10-17, #8).
    EXPECT_TRUE(engine->Play().IsError());
    EXPECT_TRUE(engine->Stop().IsError());
    EXPECT_EQ(engine->GetState(), EngineState::Uninitialized);
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

TEST(EngineStressTest, ConcurrentShutdownIsSafe) {
    // Audit 2026-10-17, #7: a second Shutdown() used to fall through and
    // re-run the teardown — double StopMonitor/Release and a second join
    // of the same std::thread (UB). The late caller now waits for the
    // first teardown to finish; both must report success.
    auto engine = CreateWallpaperEngine();
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    std::atomic<int> ready{0};
    std::atomic<bool> firstOk{false};
    std::atomic<bool> lateOk{false};
    auto callShutdown = [&](std::atomic<bool>& ok, bool late) {
        ready.fetch_add(1, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        // The late caller arrives while the first is mid-teardown (the
        // render-thread join keeps that window open well past 2 ms).
        if (late) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ok.store(engine->Shutdown().IsSuccess(), std::memory_order_release);
    };

    std::thread first(callShutdown, std::ref(firstOk), false);
    std::thread late(callShutdown, std::ref(lateOk), true);
    first.join();
    late.join();

    EXPECT_TRUE(firstOk.load());
    EXPECT_TRUE(lateOk.load());
    EXPECT_EQ(engine->GetState(), EngineState::Uninitialized);
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
    const std::string video = FindEngineTestVideo();
    if (video.empty()) {
        GTEST_SKIP() << "no test video (set REX_TEST_VIDEO)";
    }
    auto engine = CreateWallpaperEngine();
    engine->Initialize(WallpaperConfig{});

    SceneInfo scene;
    scene.id = "video_test";
    scene.sourcePath = video;
    scene.type = "video";

    EXPECT_TRUE(engine->LoadScene(scene).IsSuccess());
    EXPECT_EQ(engine->GetCurrentScene().type, "video");
    EXPECT_TRUE(engine->GetCurrentScene().sourcePath == video);

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

// Regression: the pre-fix render loop consumed the host request at READ time
// (m_recoveryNeeded.exchange(false), Engine.cpp:1836). A failed attempt
// re-arms the flag and schedules a backoff deadline; on the following pass,
// still inside the window, the exchange cleared the flag while skipping the
// attempt — the retry never ran and a host-only recovery (Explorer restart,
// no device loss) bricked the wallpaper for the session. The request must be
// consumed at attempt time only.

TEST(RecoveryBackoffTest, RequestConsumedOnlyWhenAttemptRuns) {
    using rex::wallpaper::DecideRecovery;
    // No request, or not yet at the deadline -> no attempt, nothing consumed.
    EXPECT_FALSE(DecideRecovery(false, false, true).consumeRequest);
    EXPECT_FALSE(DecideRecovery(true, false, false).consumeRequest);
    EXPECT_FALSE(DecideRecovery(true, false, false).attempt);
    // Deadline reached with a host request -> attempt runs and consumes it.
    const auto ready = DecideRecovery(true, false, true);
    EXPECT_TRUE(ready.attempt);
    EXPECT_TRUE(ready.consumeRequest);
    // Device-loss-only (no host request): attempt, but nothing to consume.
    const auto lost = DecideRecovery(false, true, true);
    EXPECT_TRUE(lost.attempt);
    EXPECT_FALSE(lost.consumeRequest);
}

TEST(RecoveryBackoffTest, FailedAttemptRetriesAfterBackoffWindow) {
    // Drive the fixed render-loop decision across three passes: attempt 1
    // fails and re-arms + backs off; pass 2 is inside the window and must
    // NOT consume the request; pass 3, deadline reached, attempts again.
    bool pending = true;
    bool ready = true;
    int attempts = 0;

    auto pass = [&]() {
        const auto d = rex::wallpaper::DecideRecovery(pending, false, ready);
        if (d.consumeRequest) pending = false;
        if (d.attempt) {
            ++attempts;
            // Failure branch: re-arm the request, schedule the deadline.
            ready = false;
            pending = true;
        }
        return d;
    };

    EXPECT_TRUE(pass().attempt);   // pass 1: first attempt runs
    EXPECT_FALSE(pass().attempt);  // pass 2: inside the backoff window
    EXPECT_TRUE(pending);          //   -> request survived (pre-fix: lost)
    ready = true;                  // deadline elapses
    EXPECT_TRUE(pass().attempt);   // pass 3: retry runs
    EXPECT_EQ(attempts, 2);

    // Pre-fix model for contrast: consuming at read time loses the retry.
    bool legacyPending = true;
    auto legacyConsumeAtRead = [&]() {
        const bool r = legacyPending;
        legacyPending = false;
        return r;
    };
    (void)legacyConsumeAtRead();  // pass 1: attempt ran, then failure re-arms
    legacyPending = true;
    (void)legacyConsumeAtRead();  // pass 2: inside window — cleared, no attempt
    EXPECT_FALSE(legacyPending);  // nothing left when the deadline passes
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

// ═══════════════════════════════════════════════════════════════════════════
// Playback-rate wall clock (Phase 4)
// ═══════════════════════════════════════════════════════════════════════════
//
// The wall target advances at the playback rate: at 2x, one wall second
// moves position ~2 s (previously it always advanced 1x while decode ran
// 2x, so position lied whenever speed != 1).

TEST(SpeedClockTest, PositionAdvancesAtPlaybackRate) {
    constexpr const char* kImage =
        "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    if (!std::filesystem::exists(kImage)) {
        GTEST_SKIP() << "stock wallpaper image missing";
    }
    // Hold the console display awake for the measurement. The engine
    // suspends the wallpaper when the OS reports GUID_CONSOLE_DISPLAY_STATE
    // as off, and this box turns the monitor off after 300 s of idle - a
    // multi-minute build/test cycle is exactly that. The engine then
    // correctly auto-pauses ("Suspend source active (session-locked=false,
    // display-off=true, power-saving=false)"), the clock freezes at the
    // first publish, and the rate measurement below reads ~0.001 s instead
    // of ~2 s. That is an environment coupling, not a rate regression: the
    // suspend sources cannot be configured off, and the pause arbitration
    // has its own coverage. ES_DISPLAY_REQUIRED keeps the display on (and
    // wakes it if it already slept), so the engine stays Running.
    const auto previousExecutionState = SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    // The ambient fullscreen detector must not pause the wallpaper
    // mid-measurement: observed once during a full-suite run — an external
    // fullscreen edge auto-paused playback right after Play(), freezing the
    // position at the first Update() publish (~0.78 ms) for the whole
    // sleep. Rate scaling is what this test measures; the pause
    // arbitration has its own coverage (PlaybackArbitrationTest /
    // FullscreenDetectionTest).
    WallpaperConfig config;
    config.pauseOnFullscreen = false;
    ASSERT_TRUE(engine->Initialize(config).IsSuccess());

    SceneInfo scene;
    scene.id = "speed_clock";
    scene.sourcePath = kImage;
    scene.type = "static";
    ASSERT_TRUE(engine->LoadScene(scene).IsSuccess());
    ASSERT_TRUE(engine->SetSpeed(2.0f).IsSuccess());
    ASSERT_TRUE(engine->Play().IsSuccess());

    // The display may have been asleep when the engine registered: the
    // "console display on" broadcast arrives a moment later and the engine
    // resumes then. Wait for Running rather than assuming it - with the
    // display held awake above, a machine with a usable display reaches
    // Running within a few hundred ms.
    bool reachedRunning = false;
    for (int i = 0; i < 60; ++i) {
        if (engine->GetState() == EngineState::Running) {
            reachedRunning = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!reachedRunning) {
        // No usable display on this machine: the engine correctly
        // auto-paused for a suspend source (console display off) and the
        // rate cannot be measured. Skip instead of reporting a false
        // failure - the same pattern this file already uses for missing
        // fixtures. The suspend/resume behaviour itself is covered by the
        // engine's own arbitration tests.
        SetThreadExecutionState(previousExecutionState);
        GTEST_SKIP() << "the engine stayed suspended (no live console display "
                        "on this machine); the playback rate cannot be measured";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const EngineState state = engine->GetState();
    const double pos = engine->GetPosition();
    // 2x rate: ~2.0 after 1 s (wide margins for CI scheduling jitter;
    // unscaled 1x would read ~1.0).
    // Suspend sources (session lock / display off / power saving) cannot be
    // configured off: name them in the failure instead of letting a frozen
    // clock masquerade as a broken rate.
    EXPECT_EQ(state, EngineState::Running)
        << "playback left Running — a suspend source auto-paused the "
           "wallpaper; the frozen clock invalidates the rate measurement";
    EXPECT_GT(pos, 1.2) << "state=" << static_cast<int>(state);
    EXPECT_LT(pos, 2.8) << "state=" << static_cast<int>(state);

    // Release the display hold (the measurement is done). A failed ASSERT
    // above returns early and leaves it held - the display staying on for
    // the rest of the suite is harmless, and it keeps the other playback
    // tests out of the same ambient trap.
    SetThreadExecutionState(previousExecutionState);

    EXPECT_TRUE(engine->Stop().IsSuccess());
    EXPECT_TRUE(engine->UnloadScene().IsSuccess());
    engine->Shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// Position clock (audit 2026-10-17, #3 — cross-thread position state)
// ═══════════════════════════════════════════════════════════════════════════

TEST(PositionClockTest, StartsAtZeroAndAdvancesWithRate) {
    PositionClock clock;
    clock.Start(100.0);
    EXPECT_TRUE(clock.Running());
    EXPECT_DOUBLE_EQ(clock.Position(105.0, 1.0), 5.0);
    EXPECT_DOUBLE_EQ(clock.Position(105.0, 2.0), 10.0);
}

TEST(PositionClockTest, FoldBanksElapsedAndStops) {
    PositionClock clock;
    clock.SetBase(3.0);
    clock.Start(100.0);
    EXPECT_DOUBLE_EQ(clock.Fold(110.0, 2.0), 23.0);  // 3 + 10 s at 2x
    EXPECT_FALSE(clock.Running());
    // Frozen after the fold: later reads ignore time and rate.
    EXPECT_DOUBLE_EQ(clock.Position(200.0, 1.0), 23.0);
    EXPECT_DOUBLE_EQ(clock.Position(200.0, 4.0), 23.0);
}

TEST(PositionClockTest, ResumeIsContinuousAcrossPause) {
    PositionClock clock;
    clock.Start(100.0);
    clock.Fold(110.0, 1.0);  // bank 10 s
    clock.Start(120.0);      // 10 s paused: not counted
    EXPECT_DOUBLE_EQ(clock.Position(125.0, 1.0), 15.0);
}

TEST(PositionClockTest, ResetKeepsBaseAndStops) {
    PositionClock clock;
    clock.SetBase(7.0);
    clock.Start(100.0);
    clock.Reset();
    EXPECT_FALSE(clock.Running());
    EXPECT_DOUBLE_EQ(clock.Position(150.0, 1.0), 7.0);
}

TEST(PositionClockTest, ConcurrentFoldsNeverDoubleCount) {
    // A user Pause racing an occlusion park must bank the lapse once: the
    // transition lock serializes read-fold-store and the loser observes
    // running == false. (Unserialized, the second fold could re-read the
    // pre-fold base and add the same lapse again.)
    for (int round = 0; round < 50; ++round) {
        PositionClock clock;
        clock.Start(0.0);
        std::thread a([&clock] { clock.Fold(1.0, 1.0); });
        std::thread b([&clock] { clock.Fold(1.0, 1.0); });
        a.join();
        b.join();
        EXPECT_DOUBLE_EQ(clock.Position(2.0, 1.0), 1.0) << "round " << round;
    }
}

TEST(PositionClockTest, ReaderSeesBoundedPositionWhileWriterTransitions) {
    // Render-thread reads race command-thread transitions; a snapshot must
    // never mix fields into an out-of-range position (e.g. new base with
    // stale running=true). Invariant for this writer loop: 0 <= pos <= 1.
    PositionClock clock;
    std::atomic<bool> done{false};
    std::thread writer([&clock, &done] {
        for (int i = 0; i < 2000; ++i) {
            clock.SetBase(0.0);
            clock.Start(0.0);
            clock.Fold(1.0, 1.0);
        }
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        const double pos = clock.Position(1.0, 1.0);
        EXPECT_GE(pos, 0.0);
        EXPECT_LE(pos, 1.0000001);
    }
    writer.join();
}
