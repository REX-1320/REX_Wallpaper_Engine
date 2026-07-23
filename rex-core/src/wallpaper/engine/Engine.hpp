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
