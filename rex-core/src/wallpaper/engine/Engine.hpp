#pragma once

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Logger.hpp>
#include <shared/UUID.hpp>
#include <shared/Stopwatch.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>
#include <filesystem>

#include "platform/windows/IDesktopHost.hpp"
#include "graphics/renderer/D3D12Renderer.hpp"
#include "wallpaper/video/VideoManager.hpp"
#include "wallpaper/audio/AudioCapture.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include <rex/wallpaper/IRendererAdapter.hpp>

namespace rex::wallpaper {

class Engine : public IWallpaperEngine {
public:
    Engine();
    ~Engine();

    shared::Result<void> Initialize(const WallpaperConfig& config) override;
    shared::Result<void> Shutdown() override;
    EngineState GetState() const override { return m_state.load(std::memory_order_acquire); }

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
    shared::Result<void> SetMute(bool mute) override;
    shared::Result<void> SetLoop(bool loop) override;
    shared::Result<void> SetSpeed(float speed) override;

    shared::Result<void> SetProperty(const std::string& property, const std::string& value) override;
    std::vector<std::string> GetSupportedProperties() const override;
    std::string GetRendererType() const override;

    shared::Result<void> EnableAudioReactive(bool enable) override;
    shared::Result<void> SetAudioSensitivity(float sensitivity) override;

    void DumpStats() const override;
    FrameSurface GetFrameSurface() const override;
    uint64_t GetFrameSlotHandle(uint32_t slot) const override;
    void GetTelemetry(EngineTelemetry& out) const override;
    shared::UUID GetSessionId() const override { return m_sessionId; }

    // ─── Per-monitor wallpapers (Phase 5) ────────────────────────────────
    shared::Result<void> SetPerMonitorWallpaper(const std::string& monitorId,
                                                const std::string& path) override;
    void SetSettingsService(
        std::shared_ptr<rex::settings::ISettingsService> settings) override;

private:
    void Update(double deltaTime);
    void SetState(EngineState state);
    void RenderLoop();
    /// P16: sleep until the next expected frame (decoder cadence clamped
    /// by maxFps, floor ~15Hz so pause/resume/unload stay responsive).
    void PaceLoop(std::chrono::high_resolution_clock::time_point frameStart);
    shared::Result<void> ReinitializeRenderer();
    shared::Result<void> HandleDeviceLoss();
    void CheckDisplayChanges();
    /// Auto-pause/resume while a full-screen app owns the foreground.
    void CheckFullScreenPause();
    /// Push the current rotation/flip to the main renderer and the
    /// per-monitor compositors.
    void ApplyTransform();
    /// Push loop+speed+volume+mute from the current scene to the video
    /// backend in one path (P1-7). Called on LOAD and inherited by
    /// device-loss recovery (which reloads through LoadScene).
    void ApplyPlaybackSettings();
    /// True when the active host provides a wallpaper layer (MultiMonitor).
    bool IsMultiMonitorMode() const {
        return m_desktopHost && m_desktopHost->GetWallpaperLayerWindow() != nullptr;
    }
    /// Add a compositor per monitor + restore persisted assignments.
    void SetupMonitorCompositors();

    // Atomic so the render thread can safely read it while the command
    // thread swaps scenes (UnloadScene/LoadScene) under the mutex.
    std::atomic<EngineState> m_state{EngineState::Uninitialized};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loop{true};
    std::atomic<float> m_speed{1.0f};
    bool m_muted = false;  // guarded by m_mutex

    SceneInfo m_currentScene;
    MonitorInfo m_currentMonitor;
    WallpaperConfig m_config;

    // ─── Transform state (rotation / flip) ──────────────────────────────
    float m_sceneRotation = 0.0f;
    bool  m_sceneFlipH = false;
    bool  m_sceneFlipV = false;

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
    // Full last-seen topology: the change predicate compares geometry,
    // identity, primary role and DPI (P0-2 / G3), not just count + size.
    std::vector<MonitorInfo> m_lastMonitors;

    // Scene lifecycle counters (for the memory-retention diagnostics)
    std::atomic<uint64_t> m_scenesLoaded{0};
    std::atomic<uint64_t> m_scenesReleased{0};

    std::unique_ptr<rex::platform::windows::IDesktopHost> m_desktopHost;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<VideoManager> m_videoManager;
    // shared_ptr (not unique): the render thread keeps a per-frame snapshot
    // so the command thread can replace the device without freeing it from
    // under a live frame.
    std::shared_ptr<AudioCapture> m_audioCapture;
    std::unique_ptr<IRendererAdapter> m_adapter;
    std::thread m_renderThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recoveryNeeded{false};
    // Recovery retry pacing (render-thread only, see RecoveryBackoff.hpp).
    // m_recoveryDelayMs is the backoff ladder position, m_nextRecoveryAttempt
    // the earliest time the next attempt may run.
    int m_recoveryDelayMs = 0;
    std::chrono::steady_clock::time_point m_nextRecoveryAttempt{};

    // ─── Dirty-frame gating & present telemetry (P16) ──────────────────
    // Set by frame adoption on the render thread and by IPC-thread
    // property setters; consumed (and cleared) only in RenderLoop.
    std::atomic<bool> m_frameDirty{false};
    std::atomic<uint64_t> m_presentCount{0};
    // Present-rate sampler (Phase 1): GetTelemetry folds presentCount
    // deltas into presentFps. Mutable: GetTelemetry is const and already
    // holds m_mutex.
    mutable uint64_t m_presentRateBase{0};
    mutable std::chrono::steady_clock::time_point m_presentRateTime{};
    mutable double m_presentFps{0.0};
    // Render-thread only: dark clear applied once per texture-less state.
    bool m_darkCleared = false;
    // ─── Static image hot-reload (render-thread internal) ─────────────
    std::string m_hotReloadPath;
    std::filesystem::file_time_type m_hotReloadLastWrite{};

    // ─── Full-screen auto-pause (render-thread internal) ──────────────
    /// Posted by CheckFullScreenPause() so the render loop can service
    /// it without calling the locking Pause()/Play() APIs from inside
    /// the render thread (which would deadlock against Shutdown()).
    ///   0 = no request pending
    ///   1 = pause requested
    ///  -1 = resume requested
    std::atomic<int> m_fullScreenRequest{0};

    /// Auto-pause state (render-thread only): true while we paused the
    /// wallpaper behind a full-screen app, so we resume only what we
    /// paused ourselves (never a user-initiated pause).
    bool m_fullScreenPaused = false;
    /// First sighting of the current full-screen episode (render-thread
    /// only). A shell overlay that flashes fullscreen for a moment must not
    /// freeze the wallpaper — the pause fires only after it persists.
    std::chrono::steady_clock::time_point m_fullScreenSince{};
    /// Position clock base (seconds). The Stopwatch restarts from zero on
    /// every Start(), so pause/resume used to rewind the reported position
    /// while the decoder kept its own PTS. The base is folded forward at
    /// every pause and zeroed with m_currentPosition; position is always
    /// base + stopwatch-elapsed. Atomic because the render thread folds it
    /// while GetPosition() reads it under m_mutex.
    std::atomic<double> m_positionBase{0.0};
};

} // namespace rex::wallpaper
