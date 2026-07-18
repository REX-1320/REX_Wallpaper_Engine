#include <windows.h>
#include <cstdio>
#include <memory>
#include <chrono>
#include <thread>

#include <shared/Logger.hpp>
#include <wallpaper/IWallpaperEngine.hpp>

using namespace rex::shared;
using namespace rex::wallpaper;

static std::unique_ptr<IWallpaperEngine> g_engine;
static bool g_running = true;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Initialize Logger
    Logger::Instance().Initialize("rex-wallpaper.log", LogLevel::Debug);
    REX_LOG_INFO("Main", "=== REX Wallpaper Engine (Phase 1) ===");
    REX_LOG_INFO("Main", "Starting application...");

    // Create Engine
    REX_LOG_INFO("Main", "Creating engine...");

    g_engine = CreateWallpaperEngine();
    if (!g_engine) {
        REX_LOG_ERROR("Main", "Failed to create engine");
        return 1;
    }

    // Initialize Engine
    WallpaperConfig config;
    config.enableHardwareAcceleration = true;
    config.maxFps = 60;
    config.enableAudioReactive = false;

    auto result = g_engine->Initialize(config);
    if (result.IsError()) {
        REX_LOG_ERROR("Main", "Failed to initialize engine: {}", result.Error().message);
        return 1;
    }

    REX_LOG_INFO("Main", "Engine initialized (Session: {})", g_engine->GetSessionId().ToString());

    // Load Scene
    SceneInfo scene;
    scene.id = "test_scene";
    scene.name = "Test Scene";
    scene.sourcePath = "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";
    scene.type = "static";
    scene.loop = true;

    result = g_engine->LoadScene(scene);
    if (result.IsError()) {
        REX_LOG_ERROR("Main", "Failed to load scene: {}", result.Error().message);
    } else {
        REX_LOG_INFO("Main", "Scene loaded: {}", scene.name);
    }

    // Assign Monitor
    result = g_engine->AssignMonitor("DISPLAY1");
    if (result.IsError()) {
        REX_LOG_ERROR("Main", "Failed to assign monitor: {}", result.Error().message);
    } else {
        REX_LOG_INFO("Main", "Monitor assigned");
    }

    // Play
    result = g_engine->Play();
    if (result.IsError()) {
        REX_LOG_ERROR("Main", "Failed to play: {}", result.Error().message);
    } else {
        REX_LOG_INFO("Main", "Playback started");
    }

    // Message Loop
    REX_LOG_INFO("Main", "Entering message loop...");

    MSG msg = {};
    while (g_running && GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Shutdown
    REX_LOG_INFO("Main", "Shutting down...");

    if (g_engine) {
        g_engine->Stop();
        g_engine->Shutdown();
        g_engine.reset();
    }

    Logger::Instance().Shutdown();
    REX_LOG_INFO("Main", "Goodbye!");

    return 0;
}
