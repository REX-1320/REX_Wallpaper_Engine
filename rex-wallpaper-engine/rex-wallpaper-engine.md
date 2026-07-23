# REX Wallpaper Engine — Source Code

Total: **16 files** — 1 public interface header, 13 engine source files, 2 plugin files.

---

## `include/rex/wallpaper/IWallpaperEngine.hpp`

```cpp
#pragma once

#include <shared/Result.hpp>
#include <shared/UUID.hpp>

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace rex::wallpaper {

// ─── Forward Declarations ──────────────────────────────────────────────────

struct SceneInfo;
struct MonitorInfo;
struct WallpaperConfig;

// ─── Enums ──────────────────────────────────────────────────────────────────

enum class EngineState {
    Uninitialized,
    Initializing,
    Initialized,
    Running,
    Paused,
    Stopped,
    Error,
    ShuttingDown
};

enum class FillMode {
    Fill,    // Scale to fill screen, crop excess (default)
    Fit,     // Scale to fit within screen, letterbox
    Stretch, // Stretch to fill screen (ignore aspect ratio)
    Center,  // Center at native resolution
    Tile     // Tile at native resolution (repeat)
};

// ─── IRuntime ──────────────────────────────────────────────────────────────

/// Base interface for all REX runtimes.
/// Every runtime (Wallpaper, WindowFX) implements this.
class IRuntime {
public:
    virtual ~IRuntime() = default;

    virtual shared::Result<void> Initialize(const WallpaperConfig& config) = 0;
    virtual shared::Result<void> Shutdown() = 0;
    virtual EngineState GetState() const = 0;
    virtual shared::UUID GetSessionId() const = 0;
};

// ─── SceneInfo ─────────────────────────────────────────────────────────────

struct SceneInfo {
    std::string id;
    std::string name;
    std::string sourcePath;
    std::string type = "static"; // "static", "video", "web", "interactive"
    bool    loop     = true;
    bool    muted    = false;
    float   volume   = 1.0f;
    float   speed    = 1.0f;
    float   brightness = 1.0f;
    float   contrast   = 1.0f;
    float   saturation = 1.0f;
    FillMode fillMode = FillMode::Fill;
};

// ─── MonitorInfo ───────────────────────────────────────────────────────────

struct MonitorInfo {
    std::string id;
    std::string name;
    int     x          = 0;
    int     y          = 0;
    int     width      = 0;
    int     height     = 0;
    bool    isPrimary  = false;
    float   dpiScale   = 1.0f;
};

// ─── WallpaperConfig ───────────────────────────────────────────────────────

struct WallpaperConfig {
    bool  enableHardwareAcceleration = true;
    int   maxFps       = 60;
    int   targetMonitor = 0;
    bool  enableAudioReactive = false;
    float audioSensitivity    = 0.5f;
};

// ─── IWallpaperEngine ──────────────────────────────────────────────────────

class IWallpaperEngine : public IRuntime {
public:
    ~IWallpaperEngine() override = default;

    // ─── Lifecycle (inherited from IRuntime) ──────────────────────────────
    // Initialize(), Shutdown(), GetState(), GetSessionId()

    // ─── Scene ────────────────────────────────────────────────────────────
    virtual shared::Result<void> LoadScene(const SceneInfo& scene) = 0;
    virtual shared::Result<void> UnloadScene() = 0;
    virtual SceneInfo GetCurrentScene() const = 0;

    // ─── Playback ─────────────────────────────────────────────────────────
    virtual shared::Result<void> Play() = 0;
    virtual shared::Result<void> Pause() = 0;
    virtual shared::Result<void> Stop() = 0;
    virtual bool   IsPlaying() const = 0;
    virtual double GetPosition() const = 0;
    virtual double GetDuration() const = 0;

    // ─── Monitor ──────────────────────────────────────────────────────────
    virtual shared::Result<void> AssignMonitor(const std::string& monitorId) = 0;
    virtual shared::Result<void> ClearMonitor() = 0;
    virtual MonitorInfo GetCurrentMonitor() const = 0;
    virtual std::vector<MonitorInfo> EnumerateMonitors() const = 0;

    // ─── Settings ─────────────────────────────────────────────────────────
    virtual shared::Result<void> SetFillMode(FillMode mode) = 0;
    virtual shared::Result<void> SetBrightness(float value) = 0;
    virtual shared::Result<void> SetContrast(float value) = 0;
    virtual shared::Result<void> SetSaturation(float value) = 0;
    virtual shared::Result<void> SetVolume(float value) = 0;
    virtual shared::Result<void> SetLoop(bool loop) = 0;
    virtual shared::Result<void> SetSpeed(float speed) = 0;

    // ─── Audio ───────────────────────────────────────────────────────────
    virtual shared::Result<void> EnableAudioReactive(bool enable) = 0;
    virtual shared::Result<void> SetAudioSensitivity(float sensitivity) = 0;

    // ─── Debug ────────────────────────────────────────────────────────────
    virtual void DumpStats() const = 0;
};

// ─── Factory ───────────────────────────────────────────────────────────────

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine();

} // namespace rex::wallpaper
```

---

## `src/wallpaper/engine/Engine.hpp`

```cpp
#pragma once

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Logger.hpp>
#include <shared/UUID.hpp>
#include <shared/Stopwatch.hpp>

#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

#include "platform/windows/IDesktopHost.hpp"
#include "graphics/renderer/D3D12Renderer.hpp"
#include "wallpaper/playback/PlaybackController.hpp"

namespace rex::wallpaper {

class Engine : public IWallpaperEngine {
public:
    Engine();
    ~Engine();

    shared::Result<void> Initialize(const WallpaperConfig& config) override;
    shared::Result<void> Shutdown() override;
    EngineState GetState() const override { return m_state; }

    shared::Result<void> LoadScene(const SceneInfo& scene) override;
    shared::Result<void> UnloadScene() override;
    SceneInfo GetCurrentScene() const override { return m_currentScene; }

    shared::Result<void> Play() override;
    shared::Result<void> Pause() override;
    shared::Result<void> Stop() override;
    bool IsPlaying() const override { return m_playing; }
    double GetPosition() const override;
    double GetDuration() const override;

    shared::Result<void> AssignMonitor(const std::string& monitorId) override;
    shared::Result<void> ClearMonitor() override;
    MonitorInfo GetCurrentMonitor() const override { return m_currentMonitor; }
    std::vector<MonitorInfo> EnumerateMonitors() const override;

    shared::Result<void> SetFillMode(FillMode mode) override;
    shared::Result<void> SetBrightness(float value) override;
    shared::Result<void> SetContrast(float value) override;
    shared::Result<void> SetSaturation(float value) override;
    shared::Result<void> SetVolume(float value) override;
    shared::Result<void> SetLoop(bool loop) override;
    shared::Result<void> SetSpeed(float speed) override;

    shared::Result<void> EnableAudioReactive(bool enable) override;
    shared::Result<void> SetAudioSensitivity(float sensitivity) override;

    void DumpStats() const override;
    shared::UUID GetSessionId() const override { return m_sessionId; }

private:
    void Update(double deltaTime);
    void SetState(EngineState state);
    void RenderLoop();
    shared::Result<void> ReinitializeRenderer();
    void CheckDisplayChanges();

    EngineState m_state = EngineState::Uninitialized;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loop{true};
    std::atomic<float> m_speed{1.0f};

    SceneInfo m_currentScene;
    MonitorInfo m_currentMonitor;
    WallpaperConfig m_config;

    shared::UUID m_sessionId;

    mutable std::mutex m_mutex;
    shared::Stopwatch m_stopwatch;
    double m_currentPosition = 0.0;

    // ─── Telemetry ─────────────────────────────────────────────────────
    double m_frameTime = 0.0;
    long long m_frameCount = 0;
    uint32_t m_recoveryCount = 0;
    uint32_t m_deviceResetCount = 0;
    int m_lastMonitorCount = 0;
    int m_lastPrimaryWidth = 0;
    int m_lastPrimaryHeight = 0;

    std::unique_ptr<rex::platform::windows::IDesktopHost> m_desktopHost;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<PlaybackController> m_playback;
    std::thread m_renderThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recoveryNeeded{false};

    // Video frame pixel buffer — persists between render frames to avoid
    // re-allocation on every Tick -> UpdateTexturePixels call.
    std::vector<uint8_t> m_videoFramePixels;
};

} // namespace rex::wallpaper
```

---

## `src/wallpaper/engine/Engine.cpp`

```cpp
#include "Engine.hpp"
#include "platform/windows/DesktopHostFactory.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>

namespace rex::wallpaper {

Engine::Engine() {
    REX_LOG_INFO("Engine", "Engine instance created");
}

Engine::~Engine() {
    if (m_state != EngineState::Uninitialized) {
        Shutdown();
    }
}

shared::Result<void> Engine::Initialize(const WallpaperConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Uninitialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized, "Engine already initialized");
    }

    REX_LOG_INFO("Engine", "Initializing...");

    m_config = config;
    m_sessionId = shared::UUID::Generate();

    if (m_config.enableHardwareAcceleration) {
        REX_LOG_INFO("Engine", "Hardware acceleration enabled");
    } else {
        REX_LOG_INFO("Engine", "Hardware acceleration disabled");
    }

    // Acquire desktop host (tries WorkerW → Progman → error)
    m_desktopHost = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
    if (!m_desktopHost) {
        REX_LOG_ERROR("Engine", "Failed to create desktop host");
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "No desktop hosting strategy available. "
            "Check the log for details on why WorkerW and "
            "Progman both failed.");
    }

    HWND renderWindow = m_desktopHost->GetRenderWindow();
    int width  = m_desktopHost->GetWidth();
    int height = m_desktopHost->GetHeight();

    // Create Renderer
    m_renderer = std::make_unique<D3D12Renderer>();
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to initialize renderer: {}", result.Error().message);
        return result;
    }

    // Start shell monitor for desktop host recovery
    // The callback runs on a background thread — it only sets a flag.
    // The actual recovery happens on the render thread.
    m_desktopHost->StartMonitor([this]() {
        m_recoveryNeeded.store(true, std::memory_order_release);
    });

    // Cache monitor state for hot-plug detection
    auto monitors = EnumerateMonitors();
    m_lastMonitorCount = static_cast<int>(monitors.size());
    for (const auto& m : monitors) {
        if (m.isPrimary) {
            m_lastPrimaryWidth  = m.width;
            m_lastPrimaryHeight = m.height;
            break;
        }
    }

    // Start render thread
    m_running = true;
    m_renderThread = std::thread(&Engine::RenderLoop, this);

    SetState(EngineState::Initialized);
    m_stopwatch.Start();

    REX_LOG_INFO("Engine", "Initialized successfully (session: {})", m_sessionId.ToString().c_str());
    return shared::Result<void>();
}

shared::Result<void> Engine::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == EngineState::Uninitialized) {
        return shared::Result<void>();
    }

    REX_LOG_INFO("Engine", "Shutting down...");

    SetState(EngineState::ShuttingDown);

    m_desktopHost->StopMonitor();
    m_running = false;
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    m_playback.reset();
    m_renderer.reset();
    m_desktopHost->Release();
    m_desktopHost.reset();

    m_playing = false;
    m_currentScene = SceneInfo{};
    m_stopwatch.Stop();

    SetState(EngineState::Uninitialized);
    REX_LOG_INFO("Engine", "Shutdown complete");
    return shared::Result<void>();
}

// ─── Recovery ───────────────────────────────────────────────────────────────

shared::Result<void> Engine::ReinitializeRenderer() {
    REX_LOG_INFO("Engine", "Reinitializing renderer...");

    // Destroy old renderer
    m_playback.reset();
    if (m_renderer) {
        m_renderer->Shutdown();
    }

    // Reacquire the desktop host if it was lost (e.g., Explorer restart).
    // If still acquired (device loss only), keep the existing host.
    if (!m_desktopHost || !m_desktopHost->IsAcquired()) {
        auto result = m_desktopHost->Acquire();
        if (result.IsError()) {
            REX_LOG_ERROR("Engine", "Failed to reacquire desktop host: {}",
                          result.Error().message);
            return result;
        }
    }

    HWND renderWindow = m_desktopHost->GetRenderWindow();
    int width  = m_desktopHost->GetWidth();
    int height = m_desktopHost->GetHeight();

    m_renderer = std::make_unique<D3D12Renderer>();
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to reinitialize renderer: {}",
                      result.Error().message);
        return result;
    }

    // Reload scene
    if (!m_currentScene.sourcePath.empty()) {
        auto loadResult = LoadScene(m_currentScene);
        if (loadResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to reload scene after recovery: {}",
                         loadResult.Error().message);
        }
    }

    REX_LOG_INFO("Engine", "Renderer reinitialized successfully");
    return shared::Result<void>();
}


shared::Result<void> Engine::LoadScene(const SceneInfo& scene) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Initialized && m_state != EngineState::Stopped) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "Engine not ready to load scene");
    }

    REX_LOG_INFO("Engine", "Loading scene: {} (type: {}, path: {})",
                 scene.name.c_str(), scene.type.c_str(), scene.sourcePath.c_str());

    m_currentScene = scene;
    m_currentPosition = 0.0;

    // Load texture / video based on scene type
    if (scene.type == "static" && m_renderer && !scene.sourcePath.empty()) {
        auto texResult = m_renderer->LoadTexture(scene.sourcePath);
        if (texResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to load texture '{}': {}",
                         scene.sourcePath.c_str(), texResult.Error().message);
        }
    } else if (scene.type == "video" && !scene.sourcePath.empty()) {
        m_playback = std::make_unique<PlaybackController>();
        auto videoResult = m_playback->Open(scene.sourcePath);
        if (videoResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to open video '{}': {}",
                         scene.sourcePath.c_str(), videoResult.Error().message);
            m_playback.reset();
        }
    }

    SetState(EngineState::Stopped);
    return shared::Result<void>();
}

shared::Result<void> Engine::UnloadScene() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Unloading scene");

    if (m_playback) {
        m_playback->Close();
        m_playback.reset();
    }

    if (m_renderer) {
        m_renderer->ClearTexture();
    }

    m_playing = false;
    m_currentScene = SceneInfo{};
    m_currentPosition = 0.0;

    SetState(EngineState::Initialized);
    return shared::Result<void>();
}

shared::Result<void> Engine::Play() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_currentScene.sourcePath.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "No scene loaded");
    }

    REX_LOG_INFO("Engine", "Play requested");

    if (m_playback && m_currentScene.type == "video") {
        m_playback->Play();
    }

    m_playing = true;
    SetState(EngineState::Running);
    m_stopwatch.Start();

    return shared::Result<void>();
}

shared::Result<void> Engine::Pause() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Running) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "Not playing");
    }

    REX_LOG_INFO("Engine", "Pause requested");

    if (m_playback) {
        m_playback->Pause();
    }

    m_playing = false;
    SetState(EngineState::Paused);
    m_stopwatch.Stop();

    return shared::Result<void>();
}

shared::Result<void> Engine::Stop() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Stop requested");

    if (m_playback) {
        m_playback->Stop();
    }

    m_playing = false;
    m_currentPosition = 0.0;
    SetState(EngineState::Stopped);
    m_stopwatch.Reset();

    return shared::Result<void>();
}

double Engine::GetPosition() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playing) {
        return m_stopwatch.ElapsedSeconds();
    }
    return m_currentPosition;
}

double Engine::GetDuration() const {
    if (m_playback && m_playback->IsOpen()) {
        return m_playback->GetDuration();
    }
    return 0.0;
}

shared::Result<void> Engine::AssignMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Assigning monitor: {}", monitorId.c_str());

    auto monitors = EnumerateMonitors();
    for (const auto& m : monitors) {
        if (m.id == monitorId || m.name == monitorId) {
            m_currentMonitor = m;
            REX_LOG_INFO("Engine", "Monitor assigned: {} ({}x{})",
                         m.name.c_str(), m.width, m.height);
            return shared::Result<void>();
        }
    }

    // Fallback: use primary monitor dimensions
    m_currentMonitor.id = monitorId;
    m_currentMonitor.name = monitorId;
    m_currentMonitor.width = GetSystemMetrics(SM_CXSCREEN);
    m_currentMonitor.height = GetSystemMetrics(SM_CYSCREEN);
    m_currentMonitor.isPrimary = true;

    REX_LOG_WARN("Engine", "Monitor '{}' not found, using fallback {}x{}",
                 monitorId.c_str(), m_currentMonitor.width, m_currentMonitor.height);

    return shared::Result<void>();
}

shared::Result<void> Engine::ClearMonitor() {
    std::lock_guard<std::mutex> lock(m_mutex);
    REX_LOG_INFO("Engine", "Clearing monitor");
    m_currentMonitor = MonitorInfo{};
    return shared::Result<void>();
}

std::vector<MonitorInfo> Engine::EnumerateMonitors() const {
    std::vector<MonitorInfo> monitors;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT rect, LPARAM lParam) -> BOOL {
            auto* vec = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);

            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hMon, &mi)) {
                return TRUE; // skip this monitor, continue enumeration
            }

            MonitorInfo info;
            info.x        = mi.rcMonitor.left;
            info.y        = mi.rcMonitor.top;
            info.width    = mi.rcMonitor.right - mi.rcMonitor.left;
            info.height   = mi.rcMonitor.bottom - mi.rcMonitor.top;
            info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            // Use wide-char name for the ID
            char nameBuf[128];
            WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
            info.id   = nameBuf;
            info.name = info.id;

            // DPI scale — use default 1.0; can be enhanced with GetDpiForMonitor
            UINT dpiX = 96, dpiY = 96;
            info.dpiScale = static_cast<float>(dpiX) / 96.0f;

            vec->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    return monitors;
}

shared::Result<void> Engine::SetFillMode(FillMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.fillMode = mode;
    if (m_renderer) {
        m_renderer->SetFillMode(mode);
    }
    REX_LOG_DEBUG("Engine", "Fill mode set to {}", static_cast<int>(mode));
    return shared::Result<void>();
}

shared::Result<void> Engine::SetBrightness(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.brightness = std::clamp(value, 0.0f, 2.0f);
    REX_LOG_DEBUG("Engine", "Brightness set to {:.2f}", m_currentScene.brightness);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetContrast(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.contrast = std::clamp(value, 0.0f, 2.0f);
    REX_LOG_DEBUG("Engine", "Contrast set to {:.2f}", m_currentScene.contrast);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSaturation(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.saturation = std::clamp(value, 0.0f, 2.0f);
    REX_LOG_DEBUG("Engine", "Saturation set to {:.2f}", m_currentScene.saturation);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetVolume(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.volume = std::clamp(value, 0.0f, 1.0f);
    REX_LOG_DEBUG("Engine", "Volume set to {:.2f}", m_currentScene.volume);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetLoop(bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loop = loop;
    REX_LOG_DEBUG("Engine", "Loop set to {}", loop ? "true" : "false");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speed = std::clamp(speed, 0.25f, 4.0f);
    REX_LOG_DEBUG("Engine", "Speed set to {:.2f}", m_speed.load());
    return shared::Result<void>();
}

shared::Result<void> Engine::EnableAudioReactive(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.enableAudioReactive = enable;
    REX_LOG_INFO("Engine", "Audio-reactive {}", enable ? "enabled" : "disabled");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetAudioSensitivity(float sensitivity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.audioSensitivity = std::clamp(sensitivity, 0.0f, 1.0f);
    REX_LOG_DEBUG("Engine", "Audio sensitivity set to {:.2f}", m_config.audioSensitivity);
    return shared::Result<void>();
}

void Engine::DumpStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "=== Engine Stats ===");
    REX_LOG_INFO("Engine", "  State:           {}", static_cast<int>(m_state));
    REX_LOG_INFO("Engine", "  Playing:         {}", m_playing ? "true" : "false");
    REX_LOG_INFO("Engine", "  Loop:            {}", m_loop ? "true" : "false");
    REX_LOG_INFO("Engine", "  Speed:           {:.2f}", m_speed.load());
    REX_LOG_INFO("Engine", "  Position:        {:.2f}s", m_currentPosition);
    REX_LOG_INFO("Engine", "  Frame:           {} (time: {:.2f}ms)",
                 m_frameCount, m_frameTime);
    REX_LOG_INFO("Engine", "  Recoveries:      {}", m_recoveryCount);
    REX_LOG_INFO("Engine", "  Device resets:   {}", m_deviceResetCount);
    REX_LOG_INFO("Engine", "  Scene:           {}", m_currentScene.name.c_str());
    REX_LOG_INFO("Engine", "  Monitor:         {} ({}x{})",
                 m_currentMonitor.id.c_str(),
                 m_currentMonitor.width, m_currentMonitor.height);

    if (m_playback) {
        auto tel = m_playback->GetTelemetry();
        REX_LOG_INFO("Engine", "  ── Playback ──────");
        REX_LOG_INFO("Engine", "  Frames decoded:  {}", tel.framesDecoded);
        REX_LOG_INFO("Engine", "  Frames consumed: {}", tel.framesConsumed);
        REX_LOG_INFO("Engine", "  Frames dropped:  {}", tel.framesDropped);
        REX_LOG_INFO("Engine", "  Decoder FPS:     {:.1f}", tel.decoderFps);
        REX_LOG_INFO("Engine", "  Render FPS:      {:.1f}", tel.renderFps);
    }
}

void Engine::Update(double deltaTime) {
    if (m_state != EngineState::Running) return;

    m_frameCount++;
    m_frameTime = deltaTime;

    if (m_playing) {
        double elapsed = GetPosition();
        m_currentPosition = elapsed;

        if (m_frameCount % 60 == 0) {
            REX_LOG_DEBUG("Engine", "Frame {}: {:.2f} s, {:.2f} ms",
                          m_frameCount, elapsed, deltaTime);
        }
    }
}

void Engine::CheckDisplayChanges() {
    auto monitors = EnumerateMonitors();
    int count = static_cast<int>(monitors.size());

    int primaryW = 0, primaryH = 0;
    for (const auto& m : monitors) {
        if (m.isPrimary) {
            primaryW = m.width;
            primaryH = m.height;
            break;
        }
    }

    bool changed = (count != m_lastMonitorCount)
                || (primaryW != m_lastPrimaryWidth)
                || (primaryH != m_lastPrimaryHeight);

    if (!changed) return;

    REX_LOG_INFO("Engine", "Display topology changed: {} monitors (was {})",
                 count, m_lastMonitorCount);

    m_lastMonitorCount = count;
    m_lastPrimaryWidth  = primaryW;
    m_lastPrimaryHeight = primaryH;

    // Update the assigned monitor dimensions if it was the primary
    if (m_currentMonitor.isPrimary) {
        m_currentMonitor.width  = primaryW;
        m_currentMonitor.height = primaryH;
    }

    // Notify renderer of new dimensions
    if (m_renderer && m_desktopHost) {
        m_renderer->Resize(
            m_desktopHost->GetWidth(),
            m_desktopHost->GetHeight());
    }
}

void Engine::SetState(EngineState state) {
    m_state = state;
    REX_LOG_DEBUG("Engine", "State changed to {}", static_cast<int>(state));
}

void Engine::RenderLoop() {
    using namespace std::chrono;

    REX_LOG_INFO("Engine", "Render loop started");
    REX_LOG_INFO("Engine", "Desktop host: type={} renderWindow={} {}×{} valid={}",
                 m_desktopHost ? std::string(m_desktopHost->GetHostType()) : "none",
                 m_desktopHost ? fmt::ptr(m_desktopHost->GetRenderWindow()) : nullptr,
                 m_desktopHost ? m_desktopHost->GetWidth() : 0,
                 m_desktopHost ? m_desktopHost->GetHeight() : 0,
                 m_desktopHost ? (m_desktopHost->IsValid() ? "yes" : "no") : "N/A");

    while (m_running) {
        auto frameStart = high_resolution_clock::now();

        // ── Periodically check for display topology changes ─────────────
        if (m_frameCount % 120 == 0) {
            CheckDisplayChanges();
        }

        // ── Desktop host / shell restart recovery ─────────────────────
        // The flag is set from the desktop host's monitor thread; we
        // handle it here on the render thread so there's no mutex
        // contention.
        if (m_recoveryNeeded.exchange(false)) {
            REX_LOG_INFO("Engine", "WorkerW recovery requested, reinitializing...");
            SetState(EngineState::Error);
            m_recoveryCount++;

            auto recoveryResult = ReinitializeRenderer();
            if (recoveryResult.IsSuccess()) {
                m_frameCount = 0;
            }
            continue;
        }

        // ── Detect device loss ─────────────────────────────────────────
        if (m_renderer && m_renderer->IsDeviceRemoved()) {
            REX_LOG_ERROR("Engine", "Device lost, attempting recovery...");
            SetState(EngineState::Error);
            m_deviceResetCount++;
            m_recoveryCount++;

            auto recoveryResult = ReinitializeRenderer();
            if (recoveryResult.IsSuccess()) {
                if (m_playing) Play();
                m_frameCount = 0;
            }
            continue;
        }

        if (m_renderer && m_renderer->IsInitialized()) {
            m_renderer->BeginFrame();

            // ── Video frame update ──────────────────────────────────────
            if (m_state == EngineState::Running && m_playback
                && m_currentScene.type == "video") {
                DecodedFrame frame = m_playback->Tick();
                if (frame.valid && m_renderer) {
                    m_renderer->UpdateTexturePixels(
                        frame.pixels.data(),
                        frame.width,
                        frame.height,
                        frame.rowPitch);
                }
            }

            // ── Clear color (animated when no texture loaded) ─────────
            if (m_state == EngineState::Running
                && !m_renderer->HasTexture()) {
                float time = static_cast<float>(GetPosition());
                float r = 0.1f + 0.4f * (0.5f + 0.5f * sin(time * 0.5f));
                float g = 0.1f + 0.4f * (0.5f + 0.5f * sin(time * 0.7f + 1.0f));
                float b = 0.1f + 0.4f * (0.5f + 0.5f * sin(time * 0.9f + 2.0f));
                m_renderer->SetClearColor(r, g, b);
            }

            m_renderer->RenderFrame();
            m_renderer->EndFrame();

            auto frameEnd = high_resolution_clock::now();
            double deltaMs = duration<double, std::milli>(
                frameEnd - frameStart).count();
            Update(deltaMs / 1000.0);
        } else {
            // Renderer not ready — wait before retrying
            std::this_thread::sleep_for(milliseconds(16));
        }
    }

    REX_LOG_INFO("Engine", "Render loop ended");
}

} // namespace rex::wallpaper
```

---

## `src/wallpaper/engine/EngineFactory.cpp`

```cpp
#include <rex/wallpaper/IWallpaperEngine.hpp>
#include "wallpaper/engine/Engine.hpp"
#include <memory>

namespace rex::wallpaper {

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine() {
    return std::make_unique<Engine>();
}

} // namespace rex::wallpaper
```

---

## `src/wallpaper/decoder/WICTextureLoader.hpp`

```cpp
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

struct TextureData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int rowPitch = 0; // bytes per row (RGBA8 = width * 4)
};

/// Decodes an image file (JPG, PNG, BMP, TIFF) via Windows Imaging Component.
/// Returns RGBA8 pixel data, or an empty TextureData on failure.
/// The calling thread must have COM initialized (COINIT_APARTMENTTHREADED or
/// COINIT_MULTITHREADED).
TextureData DecodeTextureFromFile(std::string_view path);

} // namespace rex::wallpaper
```

---

## `src/wallpaper/decoder/WICTextureLoader.cpp`

```cpp
#include "WICTextureLoader.hpp"

#include <shared/Logger.hpp>

#include <wincodec.h>
#include <wrl/client.h>

#include <codecvt>
#include <locale>

namespace rex::wallpaper {

namespace {

    // RAII COM guard — initializes COM on construction, uninitializes on destruction.
    // Skips uninit if COM was already initialized on this thread with a different mode.
    class ComGuard {
    public:
        ComGuard() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            m_initialized = SUCCEEDED(hr);
            if (!m_initialized && hr == RPC_E_CHANGED_MODE) {
                // COM already initialized with a different threading model; just use it
                m_initialized = false;
            }
        }
        ~ComGuard() {
            if (m_initialized) {
                CoUninitialize();
            }
        }
        ComGuard(const ComGuard&) = delete;
        ComGuard& operator=(const ComGuard&) = delete;

    private:
        bool m_initialized = false;
    };

    // Converts UTF-8 string_view to wide string for WIC APIs.
    std::wstring ToWide(std::string_view sv) {
        if (sv.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), result.data(), len);
        return result;
    }

} // anonymous namespace

TextureData DecodeTextureFromFile(std::string_view path) {
    TextureData result;
    ComGuard com;

    // Create WIC imaging factory
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CoCreateInstance(IWICImagingFactory) failed: {:08x}", hr);
        return result;
    }

    // Create decoder
    std::wstring wpath = ToWide(path);
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        wpath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CreateDecoderFromFilename failed: {:08x} (path: {})",
                      hr, path);
        return result;
    }

    // Get first frame
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "GetFrame(0) failed: {:08x}", hr);
        return result;
    }

    // Convert to RGBA8
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CreateFormatConverter failed: {:08x}", hr);
        return result;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "FormatConverter::Initialize failed: {:08x}", hr);
        return result;
    }

    // Get dimensions
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.rowPitch = result.width * 4; // RGBA8 = 4 bytes per pixel

    // Copy pixels
    UINT bufferSize = result.rowPitch * result.height;
    result.pixels.resize(bufferSize);

    hr = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(result.rowPitch),
        bufferSize,
        result.pixels.data());

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CopyPixels failed: {:08x}", hr);
        result.pixels.clear();
        result.width = 0;
        result.height = 0;
        result.rowPitch = 0;
        return result;
    }

    REX_LOG_INFO("WICLoader", "Decoded {}x{} image ({} bytes) from {}",
                 result.width, result.height, bufferSize, path);

    return result;
}

} // namespace rex::wallpaper
```

---

## `src/wallpaper/decoder/MediaFoundationDecoder.hpp`

```cpp
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

// Forward-declare — dxgiformat.h is lightweight and has no D3D linkage.
// This is included for DXGI_FORMAT in VideoFrame metadata.
#include <dxgiformat.h>

namespace rex::wallpaper {

// ─── VideoFrame (CPU-backed RGBA8) ─────────────────────────────────────────
//
// Metadata fields (format, timestampHns) are preserved through the decode
// pipeline so that downstream consumers can handle HDR, tone-mapping,
// or audio sync without re-probing the source stream.

struct VideoFrame {
    std::vector<uint8_t> pixels;
    int         width       = 0;
    int         height      = 0;
    int         rowPitch    = 0;
    DXGI_FORMAT format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    int64_t     timestampHns = 0;  // 100-ns units (same as MF)
    bool        valid       = false;
};

// ─── MediaFoundationDecoder ─────────────────────────────────────────────────
//
// Opens a media file and decodes video frames via Media Foundation.
// MF uses DXVA (hardware decode) internally when available.
// Output frames are converted to RGBA8 on the CPU for simplicity,
// avoiding the complexity of D3D11↔D3D12 texture sharing.
//
// CPU cost of RGBA conversion: ~2-3 ms per 1080p30 frame on modern hardware,
// well within the <5% CPU budget.

class MediaFoundationDecoder {
public:
    MediaFoundationDecoder();
    ~MediaFoundationDecoder();

    shared::Result<void> Open(std::string_view path);
    void Close();

    /// Read next frame. Returns valid=false at EOF.
    /// The caller is responsible for looping (seeking to 0).
    VideoFrame ReadFrame();

    shared::Result<void> Seek(double seconds);

    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }
    bool   IsOpen()       const { return m_open; }

private:
    Microsoft::WRL::ComPtr<IMFSourceReader> m_sourceReader;

    bool   m_open       = false;
    double m_duration    = 0.0;
    double m_frameRate   = 30.0;
    int    m_width       = 0;
    int    m_height      = 0;
    DWORD  m_streamIndex = 0;

    // Pre-allocated RGBA8 conversion buffer
    std::vector<uint8_t> m_rgbaBuffer;
};

} // namespace rex::wallpaper
```

---

## `src/wallpaper/decoder/MediaFoundationDecoder.cpp`

```cpp
#include "MediaFoundationDecoder.hpp"
#include <shared/Logger.hpp>
#include <Mfapi.h>
#include <Mfreadwrite.h>
#include <Mferror.h>
#include <codecapi.h>

#include <algorithm>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace rex::wallpaper {

namespace {

    bool EnsureMFInitialized() {
        static bool s_ok = []() -> bool {
            HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
            if (FAILED(hr)) return false;
            REX_LOG_INFO("MFDecoder", "Media Foundation initialized");
            return true;
        }();
        return s_ok;
    }

    std::wstring ToWide(std::string_view sv) {
        if (sv.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(),
                                      static_cast<int>(sv.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, sv.data(),
                            static_cast<int>(sv.size()), result.data(), len);
        return result;
    }

    // Simple NV12→RGBA conversion. NV12 layout:
    //   Y plane:  width * height bytes (1 byte per pixel, luminance)
    //   UV plane: (width/2) * (height/2) * 2 bytes (interleaved U,V)
    void ConvertNV12ToRGBA(const uint8_t* nv12, uint8_t* rgba,
                           int width, int height, int rgbaStride) {
        const uint8_t* yPlane  = nv12;
        const uint8_t* uvPlane = nv12 + (width * height);

        for (int row = 0; row < height; ++row) {
            uint8_t* dstRow = rgba + row * rgbaStride;

            for (int col = 0; col < width; ++col) {
                int uvRow = row / 2;
                int uvCol = (col / 2) * 2;

                int Y = yPlane[row * width + col];
                int U = uvPlane[uvRow * width + uvCol] - 128;
                int V = uvPlane[uvRow * width + uvCol + 1] - 128;

                // ITU-R BT.601 limited-range → full-range RGB
                int C = Y - 16;
                int R = (298 * C + 409 * V + 128) >> 8;
                int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
                int B = (298 * C + 516 * U + 128) >> 8;

                dstRow[col * 4 + 0] = static_cast<uint8_t>(std::clamp(R, 0, 255));
                dstRow[col * 4 + 1] = static_cast<uint8_t>(std::clamp(G, 0, 255));
                dstRow[col * 4 + 2] = static_cast<uint8_t>(std::clamp(B, 0, 255));
                dstRow[col * 4 + 3] = 255;
            }
        }
    }

} // anonymous namespace

MediaFoundationDecoder::MediaFoundationDecoder() = default;

MediaFoundationDecoder::~MediaFoundationDecoder() {
    Close();
}

shared::Result<void> MediaFoundationDecoder::Open(std::string_view path) {
    if (m_open) Close();

    if (!EnsureMFInitialized()) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Media Foundation failed to start");
    }

    REX_LOG_INFO("MFDecoder", "Opening: {}", path);

    std::wstring wpath = ToWide(path);

    // ── Create Source Reader ────────────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "MFCreateAttributes failed");
    }

    // Enable hardware transforms (DXVA)
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &m_sourceReader);
    if (FAILED(hr)) {
        REX_LOG_ERROR("MFDecoder", "MFCreateSourceReaderFromURL failed: {:08x} (path: {})",
                      hr, path);
        return shared::Result<void>(shared::ErrorCode::FileReadError,
                                    "Failed to open: " + std::string(path));
    }

    // ── Request RGB32 output ────────────────────────────────────────────
    // This lets MF handle all format conversion (NV12/YUY2 → RGB) internally
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

        hr = m_sourceReader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            nullptr, outType.Get());

        if (FAILED(hr)) {
            // RGB32 might not be supported by the decoder's output.
            // Fall back to NV12 and convert manually.
            REX_LOG_WARN("MFDecoder", "RGB32 not supported, falling back to NV12 + CPU convert");

            Microsoft::WRL::ComPtr<IMFMediaType> nv12Type;
            MFCreateMediaType(&nv12Type);
            nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            hr = m_sourceReader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                nullptr, nv12Type.Get());
        }
    }

    m_streamIndex = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    // ── Read metadata ───────────────────────────────────────────────────
    PROPVARIANT var = {};
    hr = m_sourceReader->GetPresentationAttribute(m_streamIndex,
                                                   MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_UI8) {
        m_duration = static_cast<double>(var.uhVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&var);

    Microsoft::WRL::ComPtr<IMFMediaType> nativeType;
    hr = m_sourceReader->GetCurrentMediaType(m_streamIndex, &nativeType);
    if (SUCCEEDED(hr)) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        m_width  = static_cast<int>(w);
        m_height = static_cast<int>(h);

        UINT32 num = 0, den = 1;
        MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &num, &den);
        if (den > 0) m_frameRate = static_cast<double>(num) / den;
    }

    // Pre-allocate RGBA buffer
    m_rgbaBuffer.resize(m_width * m_height * 4);

    m_open = true;
    REX_LOG_INFO("MFDecoder", "Opened: {}x{} @ {:.2f} fps, {:.2f}s",
                 m_width, m_height, m_frameRate, m_duration);
    return shared::Result<void>();
}

void MediaFoundationDecoder::Close() {
    if (!m_open) return;
    REX_LOG_INFO("MFDecoder", "Closing");
    m_sourceReader.Reset();
    m_open     = false;
    m_duration  = 0.0;
    m_width    = 0;
    m_height   = 0;
    m_rgbaBuffer.clear();
}

VideoFrame MediaFoundationDecoder::ReadFrame() {
    VideoFrame frame;

    if (!m_open || !m_sourceReader) return frame;

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;

    HRESULT hr = m_sourceReader->ReadSample(
        m_streamIndex, 0, &streamIndex, &flags, &timestamp, &sample);

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        REX_LOG_DEBUG("MFDecoder", "EOF");
        return frame; // valid=false signals caller to loop
    }

    if (FAILED(hr) || !sample) return frame;

    // ── Extract buffer from sample ───────────────────────────────────────
    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    hr = sample->GetBufferByIndex(0, &mediaBuffer);
    if (FAILED(hr)) return frame;

    BYTE* rawData = nullptr;
    DWORD maxLen = 0, currLen = 0;
    hr = mediaBuffer->Lock(&rawData, &maxLen, &currLen);
    if (FAILED(hr)) return frame;

    // Check the actual subtype to determine if we need conversion
    Microsoft::WRL::ComPtr<IMFMediaType> actualType;
    hr = m_sourceReader->GetCurrentMediaType(m_streamIndex, &actualType);

    GUID subtype = {};
    bool isNV12 = false;
    if (SUCCEEDED(hr)) {
        actualType->GetGUID(MF_MT_SUBTYPE, &subtype);
        isNV12 = (subtype == MFVideoFormat_NV12);
    }

    int rgbaSize = m_width * m_height * 4;
    if (m_rgbaBuffer.size() < static_cast<size_t>(rgbaSize)) {
        m_rgbaBuffer.resize(rgbaSize);
    }

    if (isNV12) {
        ConvertNV12ToRGBA(rawData, m_rgbaBuffer.data(), m_width, m_height, m_width * 4);
    } else if (subtype == MFVideoFormat_RGB32) {
        // RGB32 from MF is actually BGRX (or BGRA depending on the decoder)
        // We need to ensure RGBA order. RGB32 layout: [B][G][R][X]
        int stride = static_cast<int>(currLen) / m_height;
        for (int row = 0; row < m_height; ++row) {
            const uint8_t* src = rawData + row * stride;
            uint8_t* dst = m_rgbaBuffer.data() + row * m_width * 4;
            for (int col = 0; col < m_width; ++col) {
                dst[col * 4 + 0] = src[col * 4 + 2]; // R
                dst[col * 4 + 1] = src[col * 4 + 1]; // G
                dst[col * 4 + 2] = src[col * 4 + 0]; // B
                dst[col * 4 + 3] = 255;               // A
            }
        }
    } else {
        // Try raw copy (might be wrong byte order, but better than nothing)
        memcpy(m_rgbaBuffer.data(), rawData, std::min(static_cast<DWORD>(rgbaSize), currLen));
    }

    mediaBuffer->Unlock();

    frame.pixels       = m_rgbaBuffer;
    frame.width        = m_width;
    frame.height       = m_height;
    frame.rowPitch     = m_width * 4;
    frame.format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    frame.timestampHns = static_cast<int64_t>(timestamp);
    frame.valid        = true;

    return frame;
}

shared::Result<void> MediaFoundationDecoder::Seek(double seconds) {
    if (!m_open || !m_sourceReader) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
                                    "No media file open");
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10000000.0);

    HRESULT hr = m_sourceReader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    if (FAILED(hr)) {
        REX_LOG_WARN("MFDecoder", "Seek to {}s failed: {:08x}", seconds, hr);
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Seek failed");
    }
    return shared::Result<void>();
}

} // namespace rex::wallpaper
```

---

## `src/wallpaper/playback/PlaybackController.hpp`

```cpp
#pragma once

#include "wallpaper/decoder/MediaFoundationDecoder.hpp"

#include <shared/Result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::wallpaper {

// ─── PlaybackTelemetry ─────────────────────────────────────────────────────

struct PlaybackTelemetry {
    uint64_t framesDecoded   = 0;  // total frames produced by decoder thread
    uint64_t framesConsumed  = 0;  // total frames consumed by Tick()
    uint64_t framesDropped   = 0;  // frames overwritten before being consumed
    double   decoderFps      = 0.0; // smoothed decode rate
    double   renderFps       = 0.0; // smoothed render consume rate
    double   uploadLatencyMs = 0.0; // time from decode → GPU upload start
    uint32_t recoveryCount   = 0;
    uint32_t deviceResets    = 0;
};

// ─── DecodedFrame ─────────────────────────────────────────────────────────
//
// Metadata is preserved from the decoder so downstream consumers can
// handle HDR, tone-mapping, or a/v sync without re-probing the source.

struct DecodedFrame {
    std::vector<uint8_t> pixels;
    int         width       = 0;
    int         height      = 0;
    int         rowPitch    = 0;
    DXGI_FORMAT format      = DXGI_FORMAT_UNKNOWN;
    int64_t     timestampHns = 0;  // 100-ns units
    bool        valid       = false;
};

// ─── PlaybackController ──────────────────────────────────────────────────
//
// Decodes video frames on a background thread and transfers them to the
// render thread via a lock-free triple-buffered ring buffer.
//
// ─── Threading model ────────────────────────────────────────────────────
//
// **SINGLE PRODUCER, SINGLE CONSUMER (SPSC).**
//
// This class assumes exactly one decoder thread (producer) and exactly one
// render/consumer thread. The ring buffer synchronization relies on this
// SPSC guarantee — the producer writes to slot (wc % kNumRingSlots) and
// the consumer reads from slot ((wc - 1) % kNumRingSlots). With the SPSC
// invariant, these are always distinct indices, so no mutex is needed on
// the hot path — only atomic load-acquire / store-release on m_writeCount.
//
// **If multiple decoders or consumers are ever needed, the ring buffer
// synchronization MUST be redesigned** (e.g., one ring per decoder + a
// mux, or a multi-producer queue).
//
// ─── Ring buffer safety ─────────────────────────────────────────────────
//
// With 3 slots, producer writes to slot (wc % 3) and consumer reads from
// slot ((wc - 1) % 3). These are always distinct indices, preventing torn
// reads. The producer may overwrite a slot the consumer hasn't read yet,
// which is tracked as a dropped frame in telemetry — the consumer simply
// picks up the next-latest frame.
//
// ─── Shared state access ────────────────────────────────────────────────
//
// Play():             writes m_playing, m_paused (atomic, no lock)
// Pause():            writes m_paused (atomic, no lock)
// Stop():             joins decoder thread under m_threadMutex
// Tick():             atomic load of m_writeCount + direct read (RC, no lock)
// Open() / Close():   exclusive under m_threadMutex

class PlaybackController {
public:
    PlaybackController();
    ~PlaybackController();

    // ─── Lifecycle ──────────────────────────────────────────────────────
    shared::Result<void> Open(std::string_view path);
    void Close();

    // ─── Playback Control ───────────────────────────────────────────────
    void Play();
    void Pause();
    void Stop();

    // ─── Frame Tick ─────────────────────────────────────────────────────
    /// Returns the latest decoded frame, or an invalid frame if no new
    /// frame has been decoded since the last call. Never blocks.
    DecodedFrame Tick();

    shared::Result<void> Seek(double seconds);

    // ─── State ──────────────────────────────────────────────────────────
    bool   IsPlaying() const { return m_playing.load(); }
    bool   IsPaused()  const { return m_paused.load(); }
    double GetPosition() const;
    double GetDuration() const { return m_duration; }
    int    GetWidth()  const { return m_width; }
    int    GetHeight() const { return m_height; }
    bool   IsOpen()    const { return m_open.load(); }

    // ─── Telemetry ──────────────────────────────────────────────────────
    PlaybackTelemetry GetTelemetry() const { return m_telemetry; }

private:
    void DecoderThread();

    std::unique_ptr<MediaFoundationDecoder> m_decoder;

    // ─── Ring Buffer (SPSC, 3 slots) ────────────────────────────────────
    static constexpr int kNumRingSlots = 3;
    DecodedFrame          m_ring[kNumRingSlots];
    std::atomic<uint64_t> m_writeCount{0};
    uint64_t              m_lastReadCount = 0;   // consumer-local

    // ─── Shared State ───────────────────────────────────────────────────
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_looping{true};
    std::atomic<float> m_speed{1.0f};
    std::atomic<double> m_currentPosition{0.0};

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    double m_frameInterval = 0.0;
    int    m_width     = 0;
    int    m_height    = 0;

    // ─── Telemetry ──────────────────────────────────────────────────────
    PlaybackTelemetry m_telemetry{};
    // Smoothed FPS tracking
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point m_fpsStartTime;
    uint64_t m_decoderFrameCount  = 0;
    uint64_t m_consumerFrameCount = 0;

    // ─── Thread Management ──────────────────────────────────────────────
    std::atomic<bool> m_threadRunning{false};
    std::thread m_decoderThread;

    std::mutex m_threadMutex;
};

} // namespace rex::wallpaper
```

---

## `src/wallpaper/playback/PlaybackController.cpp`

```cpp
#include "PlaybackController.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper {

PlaybackController::PlaybackController()
    : m_decoder(std::make_unique<MediaFoundationDecoder>()) {
    m_fpsStartTime = Clock::now();
}

PlaybackController::~PlaybackController() {
    Close();
}

shared::Result<void> PlaybackController::Open(std::string_view path) {
    std::lock_guard lock(m_threadMutex);

    if (m_decoder->IsOpen()) {
        m_threadRunning = false;
        if (m_decoderThread.joinable()) {
            m_decoderThread.join();
        }
        m_decoder->Close();
    }

    auto result = m_decoder->Open(path);
    if (result.IsError()) {
        return result;
    }

    m_width        = m_decoder->GetWidth();
    m_height       = m_decoder->GetHeight();
    m_frameRate    = m_decoder->GetFrameRate();
    m_frameInterval = (m_frameRate > 0.0) ? (1.0 / m_frameRate) : (1.0 / 30.0);
    m_duration     = m_decoder->GetDuration();
    m_writeCount.store(0, std::memory_order_relaxed);
    m_lastReadCount = 0;
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_open.store(true, std::memory_order_release);

    int frameBytes = m_width * m_height * 4;
    for (int i = 0; i < kNumRingSlots; ++i) {
        m_ring[i].pixels.reserve(static_cast<size_t>(frameBytes));
        m_ring[i].width      = m_width;
        m_ring[i].height     = m_height;
        m_ring[i].rowPitch   = m_width * 4;
        m_ring[i].format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    REX_LOG_INFO("Playback", "Opened video: {}x{} @ {:.2f} fps, {:.2f}s, {}B/frame",
                 m_width, m_height, m_frameRate, m_duration, frameBytes);
    return shared::Result<void>();
}

void PlaybackController::Close() {
    std::lock_guard lock(m_threadMutex);

    m_playing.store(false, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_open.store(false, std::memory_order_relaxed);
    m_threadRunning = false;

    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    if (m_decoder) {
        m_decoder->Close();
    }

    m_currentPosition.store(0.0, std::memory_order_relaxed);
    REX_LOG_INFO("Playback", "Closed");
}

void PlaybackController::Play() {
    if (!m_open.load()) return;

    m_playing.store(true, std::memory_order_release);
    m_paused.store(false, std::memory_order_relaxed);
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_writeCount.store(0, std::memory_order_relaxed);
    m_lastReadCount = 0;

    // Reset telemetry for new playback session
    m_telemetry = PlaybackTelemetry{};
    m_decoderFrameCount  = 0;
    m_consumerFrameCount = 0;
    m_fpsStartTime = Clock::now();

    {
        std::lock_guard lock(m_threadMutex);
        if (!m_threadRunning && m_decoder && m_decoder->IsOpen()) {
            m_threadRunning = true;
            m_decoderThread = std::thread(&PlaybackController::DecoderThread, this);
            REX_LOG_INFO("Playback", "Decoder thread started");
        }
    }

    REX_LOG_INFO("Playback", "Playing");
}

void PlaybackController::Pause() {
    m_paused.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Playback", "Paused");
}

void PlaybackController::Stop() {
    m_playing.store(false, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_threadRunning = false;

    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    if (m_decoder && m_decoder->IsOpen()) {
        m_decoder->Seek(0.0);
    }

    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_writeCount.store(0, std::memory_order_relaxed);
    m_lastReadCount = 0;
    REX_LOG_INFO("Playback", "Stopped");
}

double PlaybackController::GetPosition() const {
    // Convert 100-ns timestamp to seconds for the public API
    return m_currentPosition.load(std::memory_order_relaxed);
}

// ─── Consumer (render thread) ────────────────────────────────────────────

DecodedFrame PlaybackController::Tick() {
    uint64_t wc = m_writeCount.load(std::memory_order_acquire);

    if (wc == 0 || wc == m_lastReadCount) {
        // No new frame since last Tick (SPSC invariant: this means the
        // producer hasn't written to our slot yet).
        return DecodedFrame{};
    }

    // ── Track dropped frames ──────────────────────────────────────────────
    // wc - m_lastReadCount > 1 means we missed at least one frame because
    // the decoder produced faster than we consumed.
    uint64_t skipped = (wc - m_lastReadCount) - 1;
    if (skipped > 0) {
        m_telemetry.framesDropped += skipped;
    }

    // Read slot (wc - 1) % kNumRingSlots — guaranteed distinct from the
    // producer's current slot (wc % 3) under the SPSC invariant.
    uint64_t slot = (wc - 1) % kNumRingSlots;
    const DecodedFrame& src = m_ring[slot];

    // Deep-copy pixel data (~8 MB for 1080p). The caller (Engine render
    // loop) must memcpy this into a GPU upload buffer anyway, so the
    // extra copy is unavoidable at this architecture level.
    DecodedFrame result = src;

    m_telemetry.framesConsumed++;
    m_consumerFrameCount++;
    m_lastReadCount = wc;

    // Update consumer FPS (periodically)
    auto now = Clock::now();
    double elapsed = std::chrono::duration<double>(now - m_fpsStartTime).count();
    if (elapsed >= 2.0) {
        m_telemetry.renderFps = static_cast<double>(m_consumerFrameCount) / elapsed;
        m_telemetry.decoderFps = static_cast<double>(m_decoderFrameCount) / elapsed;
        m_consumerFrameCount = 0;
        m_decoderFrameCount  = 0;
        m_fpsStartTime = now;
    }

    return result;
}

shared::Result<void> PlaybackController::Seek(double seconds) {
    std::lock_guard lock(m_threadMutex);

    if (!m_decoder || !m_decoder->IsOpen()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "No media open");
    }

    bool wasRunning = m_threadRunning.load();
    m_threadRunning = false;
    if (m_decoderThread.joinable()) {
        m_decoderThread.join();
    }

    auto result = m_decoder->Seek(seconds);
    if (result.IsSuccess()) {
        m_currentPosition.store(seconds, std::memory_order_relaxed);
        m_writeCount.store(0, std::memory_order_relaxed);
        m_lastReadCount = 0;
    }

    if (wasRunning) {
        m_threadRunning = true;
        m_decoderThread = std::thread(&PlaybackController::DecoderThread, this);
    }

    return result;
}

// ─── Producer (decoder thread) ───────────────────────────────────────────

void PlaybackController::DecoderThread() {
    using namespace std::chrono;

    REX_LOG_INFO("Playback", "Decoder thread running");

    auto lastFrameTime = Clock::now();
    m_decoder->Seek(m_currentPosition.load(std::memory_order_relaxed));
    bool eof = false;

    while (m_threadRunning.load() && !eof) {
        if (m_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(milliseconds(10));
            continue;
        }

        // ── Pace to video frame rate ───────────────────────────────────
        double speed = m_speed.load(std::memory_order_relaxed);
        double interval = m_frameInterval;
        if (speed > 0.01f) {
            interval /= static_cast<double>(speed);
        }

        auto now = Clock::now();
        double elapsed = duration<double>(now - lastFrameTime).count();
        if (elapsed < interval) {
            double sleepMs = (interval - elapsed) * 1000.0;
            if (sleepMs > 0.5) {
                std::this_thread::sleep_for(milliseconds(static_cast<int>(sleepMs)));
            }
            continue;
        }
        lastFrameTime = now;

        // ── Decode one frame ───────────────────────────────────────────
        VideoFrame videoFrame = m_decoder->ReadFrame();

        if (!videoFrame.valid) {
            // EOF — restart or stop
            if (m_looping.load(std::memory_order_relaxed)) {
                m_decoder->Seek(0.0);
                videoFrame = m_decoder->ReadFrame();
                if (!videoFrame.valid) {
                    REX_LOG_WARN("Playback", "EOF loop: decoder returned invalid frame");
                    continue;
                }
            } else {
                eof = true;
                m_playing.store(false, std::memory_order_release);
                continue;
            }
        }

        // ── Publish to ring buffer ─────────────────────────────────────
        // Under the SPSC invariant, the producer writes to slot (wc % 3)
        // while the consumer only reads from slot ((wc - 1) % 3), so
        // these indices are always distinct. No atomic CAS needed.
        uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
        int slot = static_cast<int>(wc % kNumRingSlots);

        {
            DecodedFrame& dst = m_ring[slot];

            // Move pixel data — avoids a large vector reallocation
            dst.pixels       = std::move(videoFrame.pixels);
            dst.width        = videoFrame.width;
            dst.height       = videoFrame.height;
            dst.rowPitch     = videoFrame.rowPitch;
            dst.format       = videoFrame.format;
            dst.timestampHns = videoFrame.timestampHns;
            dst.valid        = true;

            if (dst.pixels.empty() && dst.width > 0 && dst.height > 0) {
                dst.pixels.reserve(static_cast<size_t>(dst.width * dst.height * 4));
            }
        }

        // Release: all ring slot writes are visible before the counter
        // increment reaches the consumer.
        m_writeCount.store(wc + 1, std::memory_order_release);

        // Track current position in seconds (from 100-ns timestamp)
        double posSec = static_cast<double>(videoFrame.timestampHns) / 10000000.0;
        m_currentPosition.store(posSec, std::memory_order_relaxed);

        m_telemetry.framesDecoded++;
        m_decoderFrameCount++;
    }

    REX_LOG_INFO("Playback", "Decoder thread ended (eof={})", eof);
}

} // namespace rex::wallpaper
```

---

## `src/wallpaper/loader/ImageLoader.hpp`

```cpp
#pragma once

#include "graphics/resource/TextureCache.hpp"

#include <shared/Result.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::wallpaper::loader {

// ─── SupportedFormats ──────────────────────────────────────────────────────
//
// Image formats decodable by WIC (Windows Imaging Component).
// Codec availability depends on the Windows version and installed codecs.

enum class ImageFormat {
    Unknown,
    PNG,
    JPEG,
    BMP,
    TIFF,
    WEBP,  // requires Windows 10 1809+
    GIF,
    ICO,
};

// ─── ImageLoader ────────────────────────────────────────────────────────────
//
// Loads and caches wallpaper images. Provides:
//
//   - Async loading with callback
//   - Format detection / validation
//   - Hot-reload via periodic polling (file modification time)
//   - Texture cache integration
//
// ## Hot-reload
//
// Call StartHotReload() with a polling interval. The loader will check
// all cached images for file changes. When a change is detected, the
// image is reloaded and the changeCallback is invoked.
//
// ## Thread safety
//
// The underlying TextureCache is thread-safe. ImageLoader itself must
// be used from a single thread for Start/Stop/Reload control, but
// GetOrLoad can be called from any thread.

class ImageLoader {
public:
    /// Called when a hot-reload detects a changed file.
    /// The path is the file that changed.
    using ChangeCallback = std::function<void(std::string_view path)>;

    ImageLoader();
    ~ImageLoader();

    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // ── Loading ─────────────────────────────────────────────────────────

    /// Load an image, caching if successful. Returns the decoded pixel
    /// data. Supported formats: PNG, JPEG, BMP, TIFF, WEBP, GIF, ICO.
    shared::Result<const graphics::CachedTexture*> Load(
        std::string_view path);

    /// Reload an image from disk (for hot-reload or explicit refresh).
    shared::Result<const graphics::CachedTexture*> Reload(
        std::string_view path);

    /// Evict an image from the cache.
    void Unload(std::string_view path);

    /// Evict all cached images.
    void Clear();

    // ── Queries ─────────────────────────────────────────────────────────

    /// Check whether an image is cached.
    bool IsLoaded(std::string_view path) const;

    /// Get cached texture data, or nullptr.
    const graphics::CachedTexture* Get(std::string_view path) const;

    /// Detect the image format from file extension.
    static ImageFormat DetectFormat(std::string_view path);

    /// Whether the format is supported.
    static bool IsFormatSupported(std::string_view path);

    /// Number of cached images.
    size_t CacheSize() const;

    // ── Hot-reload ──────────────────────────────────────────────────────

    /// Start hot-reload polling. Checks all cached files for changes
    /// every `intervalMs` milliseconds. When a change is detected,
    /// reloads the file and invokes `onChange`.
    void StartHotReload(std::chrono::milliseconds intervalMs,
                        ChangeCallback onChange);

    /// Stop hot-reload polling.
    void StopHotReload();

    /// Manually poll for changed files. Reloads changed files and
    /// returns their paths. Does not invoke the change callback.
    std::vector<std::string> PollChanges();

private:
    graphics::TextureCache m_cache;

    // Hot-reload
    std::atomic<bool> m_hotReloadRunning{false};
    std::thread m_hotReloadThread;
    ChangeCallback m_changeCallback;
    std::chrono::milliseconds m_pollInterval{2000};
};

} // namespace rex::wallpaper::loader
```

---

## `src/wallpaper/loader/ImageLoader.cpp`

```cpp
#include "ImageLoader.hpp"

#include <shared/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace rex::wallpaper::loader {

namespace {

    std::string Extension(std::string_view path) {
        auto dot = path.rfind('.');
        if (dot == std::string_view::npos) return "";
        std::string ext(path.substr(dot));
        // lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return ext;
    }

} // anonymous namespace

// ─── Construction / Destruction ─────────────────────────────────────────────

ImageLoader::ImageLoader() = default;

ImageLoader::~ImageLoader() {
    StopHotReload();
}

// ─── Loading ────────────────────────────────────────────────────────────────

shared::Result<const graphics::CachedTexture*> ImageLoader::Load(
    std::string_view path)
{
    if (path.empty()) {
        return shared::Result<const graphics::CachedTexture*>(
            shared::ErrorCode::InvalidArgument,
            "Empty path");
    }

    if (!IsFormatSupported(path)) {
        return shared::Result<const graphics::CachedTexture*>(
            shared::ErrorCode::UnsupportedFormat,
            "Unsupported image format: " + std::string(path));
    }

    return m_cache.GetOrLoad(path);
}

shared::Result<const graphics::CachedTexture*> ImageLoader::Reload(
    std::string_view path)
{
    return m_cache.Reload(path);
}

void ImageLoader::Unload(std::string_view path) {
    m_cache.Evict(path);
}

void ImageLoader::Clear() {
    m_cache.Clear();
}

// ─── Queries ────────────────────────────────────────────────────────────────

bool ImageLoader::IsLoaded(std::string_view path) const {
    return m_cache.Contains(path);
}

const graphics::CachedTexture* ImageLoader::Get(
    std::string_view path) const
{
    return m_cache.Get(path);
}

ImageFormat ImageLoader::DetectFormat(std::string_view path) {
    std::string ext = Extension(path);

    if (ext == ".png")  return ImageFormat::PNG;
    if (ext == ".jpg" || ext == ".jpeg") return ImageFormat::JPEG;
    if (ext == ".bmp")  return ImageFormat::BMP;
    if (ext == ".tiff" || ext == ".tif") return ImageFormat::TIFF;
    if (ext == ".webp") return ImageFormat::WEBP;
    if (ext == ".gif")  return ImageFormat::GIF;
    if (ext == ".ico")  return ImageFormat::ICO;

    return ImageFormat::Unknown;
}

bool ImageLoader::IsFormatSupported(std::string_view path) {
    return DetectFormat(path) != ImageFormat::Unknown;
}

size_t ImageLoader::CacheSize() const {
    return m_cache.Size();
}

// ─── Hot-reload ─────────────────────────────────────────────────────────────

void ImageLoader::StartHotReload(
    std::chrono::milliseconds intervalMs,
    ChangeCallback onChange)
{
    if (m_hotReloadRunning.load()) return;

    m_changeCallback = std::move(onChange);
    m_pollInterval   = intervalMs;
    m_hotReloadRunning.store(true, std::memory_order_release);

    m_hotReloadThread = std::thread([this]() {
        REX_LOG_INFO("ImageLoader", "Hot-reload started ({}ms)",
                     m_pollInterval.count());

        while (m_hotReloadRunning.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(m_pollInterval);

            auto changed = m_cache.PollChangedFiles();
            for (const auto& path : changed) {
                auto result = m_cache.Reload(path);
                if (result.IsSuccess() && m_changeCallback) {
                    REX_LOG_INFO("ImageLoader",
                        "Hot-reload detected change: {}", path);
                    m_changeCallback(path);
                }
            }
        }

        REX_LOG_INFO("ImageLoader", "Hot-reload stopped");
    });
}

void ImageLoader::StopHotReload() {
    if (!m_hotReloadRunning.load()) return;

    m_hotReloadRunning.store(false, std::memory_order_release);
    if (m_hotReloadThread.joinable()) {
        m_hotReloadThread.join();
    }
}

std::vector<std::string> ImageLoader::PollChanges() {
    auto changed = m_cache.PollChangedFiles();
    for (const auto& path : changed) {
        m_cache.Reload(path);
    }
    return changed;
}

} // namespace rex::wallpaper::loader
```

---

## `src/wallpaper/scene/StaticImageScene.hpp`

```cpp
#pragma once

#include "graphics/resource/TextureCache.hpp"
#include "wallpaper/loader/ImageLoader.hpp"

#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <shared/Result.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace rex::wallpaper::scene {

// ─── StaticImageScene ───────────────────────────────────────────────────────
//
// Manages a single static image wallpaper.
//
// Owns the scene lifecycle:
//   1. Load(imagePath)  — decode image, upload to GPU
//   2. ApplyTo(renderer) — configure the renderer's texture + fill mode
//   3. Unload()         — release GPU texture, evict from cache
//
// ## Fill modes
//
//   Fill    — scale to cover viewport, crop excess (default)
//   Fit     — scale to fit within viewport, letterbox
//   Stretch — stretch to fill viewport, ignore aspect ratio
//   Center  — center at native resolution, no scaling
//
// ## Hot-reload
//
// When the image file is modified on disk, call Reload() to re-decode
// and re-upload to GPU. The renderer will pick up the new texture on
// the next frame.
//
// ## Thread safety
//
// Load/Unload/Reload must be called from the engine's render thread.
// The underlying ImageLoader is thread-safe for concurrent loads.

class StaticImageScene {
public:
    StaticImageScene(std::shared_ptr<loader::ImageLoader> imageLoader);
    ~StaticImageScene();

    StaticImageScene(const StaticImageScene&) = delete;
    StaticImageScene& operator=(const StaticImageScene&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// Load the image at `path`. Decodes via WIC and caches pixel data.
    shared::Result<void> Load(std::string_view path);

    /// Reload from disk (hot-reload).
    shared::Result<void> Reload();

    /// Release GPU resources.
    void Unload();

    // ── State ──────────────────────────────────────────────────────────

    bool IsLoaded() const { return m_loaded; }
    std::string_view GetPath() const { return m_path; }
    int GetImageWidth()  const { return m_width; }
    int GetImageHeight() const { return m_height; }
    FillMode GetFillMode() const { return m_fillMode; }

    // ── Settings ───────────────────────────────────────────────────────

    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    void SetBrightness(float value) { m_brightness = value; }
    void SetContrast(float value)   { m_contrast = value; }
    void SetSaturation(float value) { m_saturation = value; }

    float GetBrightness() const { return m_brightness; }
    float GetContrast()   const { return m_contrast; }
    float GetSaturation() const { return m_saturation; }

    // ── Pixel data ─────────────────────────────────────────────────────

    /// Raw pixel data for GPU upload. Valid after Load().
    const uint8_t* GetPixels()   const;
    int            GetStride()   const;
    int            GetRowPitch() const;

    /// Version counter, incremented on each reload.
    /// The renderer can track this to detect when re-upload is needed.
    uint64_t GetVersion() const { return m_version; }

private:
    std::shared_ptr<loader::ImageLoader> m_imageLoader;
    std::string m_path;
    bool m_loaded = false;

    int m_width  = 0;
    int m_height = 0;

    FillMode m_fillMode    = FillMode::Fill;
    float m_brightness     = 1.0f;
    float m_contrast       = 1.0f;
    float m_saturation     = 1.0f;

    // Pixel cache pointer (owned by TextureCache)
    const graphics::CachedTexture* m_texture = nullptr;
    uint64_t m_version = 0;
};

} // namespace rex::wallpaper::scene
```

---

## `src/wallpaper/scene/StaticImageScene.cpp`

```cpp
#include "StaticImageScene.hpp"

#include <shared/Logger.hpp>

namespace rex::wallpaper::scene {

// ─── Construction / Destruction ─────────────────────────────────────────────

StaticImageScene::StaticImageScene(
    std::shared_ptr<loader::ImageLoader> imageLoader)
    : m_imageLoader(std::move(imageLoader))
{
}

StaticImageScene::~StaticImageScene() {
    Unload();
}

// ─── Load ───────────────────────────────────────────────────────────────────

shared::Result<void> StaticImageScene::Load(std::string_view path) {
    if (path.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
            "Empty image path");
    }

    REX_LOG_INFO("StaticImageScene", "Loading: {}", path);

    auto result = m_imageLoader->Load(path);
    if (result.IsError()) {
        return shared::Result<void>(
            result.Error().code, result.Error().message);
    }

    m_texture = result.Value();
    m_width   = m_texture->pixels.width;
    m_height  = m_texture->pixels.height;
    m_version = m_texture->version;
    m_path    = path;
    m_loaded  = true;

    REX_LOG_INFO("StaticImageScene",
        "Loaded {}x{} image (v{})", m_width, m_height, m_version);

    return shared::Result<void>();
}

// ─── Reload ─────────────────────────────────────────────────────────────────

shared::Result<void> StaticImageScene::Reload() {
    if (m_path.empty()) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
            "No image loaded");
    }

    auto result = m_imageLoader->Reload(m_path);
    if (result.IsError()) {
        return shared::Result<void>(
            result.Error().code, result.Error().message);
    }

    m_texture = result.Value();
    m_width   = m_texture->pixels.width;
    m_height  = m_texture->pixels.height;
    m_version = m_texture->version;

    REX_LOG_INFO("StaticImageScene",
        "Reloaded {}x{} (v{})", m_width, m_height, m_version);

    return shared::Result<void>();
}

// ─── Unload ─────────────────────────────────────────────────────────────────

void StaticImageScene::Unload() {
    m_loaded  = false;
    m_texture = nullptr;
    m_width   = 0;
    m_height  = 0;
    m_path.clear();
}

// ─── Pixel data access ─────────────────────────────────────────────────────

const uint8_t* StaticImageScene::GetPixels() const {
    if (!m_texture || !m_loaded) return nullptr;
    return m_texture->pixels.pixels.data();
}

int StaticImageScene::GetStride() const {
    if (!m_texture || !m_loaded) return 0;
    // RGBA8 = 4 bytes per pixel
    return 4;
}

int StaticImageScene::GetRowPitch() const {
    if (!m_texture || !m_loaded) return 0;
    return m_texture->pixels.rowPitch;
}

} // namespace rex::wallpaper::scene
```

---

## `plugins/wallpaper/wallpaper_plugin.cpp`

```cpp
#include <cstring>
#include <iostream>
#include "modules/interfaces/IModule.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "modules/ModuleSignature.hpp"

using namespace rex::modules;

class WallpaperPlugin : public IModule {
public:
    WallpaperPlugin() {
        meta_.id = "rex.wallpaper";
        meta_.name = "Wallpaper";
        meta_.version = "1.0.0";
        meta_.author = "REX Team";
        meta_.description = "Animated wallpaper renderer";
        meta_.category = "Desktop";
        meta_.builtIn = false;
        meta_.autoStart = false;
        meta_.dependencies = {"rex.graphics"};
    }

    bool initialize(IModuleContext* ctx) override {
        std::cout << "[WallpaperPlugin] init\n";
        return true;
    }

    bool start() override {
        std::cout << "[WallpaperPlugin] start\n";
        return true;
    }

    void stop() override {
        std::cout << "[WallpaperPlugin] stop\n";
    }

    void shutdown() override {
        std::cout << "[WallpaperPlugin] shutdown\n";
    }

    const ModuleMetadata& getMetadata() const override { return meta_; }
    ModuleState getState() const override { return ModuleState::Running; }

private:
    ModuleMetadata meta_;
};

extern "C" {

const ModuleSignature* GetModuleSignature() {
    static ModuleSignature sig{"rex.wallpaper", "1.0.0", "1.0", "", 1};
    return &sig;
}

IModule* CreateModule() {
    return new WallpaperPlugin();
}

}
```

---

## `plugins/wallpaper/CMakeLists.txt`

```cmake
add_library(wallpaper_plugin SHARED wallpaper_plugin.cpp)
target_include_directories(wallpaper_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(wallpaper_plugin PRIVATE $<TARGET_PROPERTY:rex-core,COMPILE_DEFINITIONS>)
```
