#pragma once

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Logger.hpp>
#include <shared/UUID.hpp>

#include <atomic>                             
#include <chrono>                             
#include <condition_variable>                 
#include <mutex>                              
#include <memory>                             
#include <thread>
#include <vector>
#include <filesystem>

#include "platform/windows/IDesktopHost.hpp"
#include "graphics/renderer/D3D12Renderer.hpp"
#include "wallpaper/video/VideoManager.hpp"
#include "wallpaper/engine/OcclusionPolicy.hpp"
#include "wallpaper/engine/PositionClock.hpp"
#include "wallpaper/playback/StageTimer.hpp"
#include <shared/PacingServo.hpp>
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
    // Copied under m_mutex: the render thread rewrites m_currentScene
    // (recovery-driven LoadScene runs on it) while command/telemetry
    // threads read it (audit 2026-10-17, #4).
    SceneInfo GetCurrentScene() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentScene;
    }

    shared::Result<void> Play() override;
    shared::Result<void> Pause() override;
    shared::Result<void> Stop() override;
    bool IsPlaying() const override { return m_playing; }
    double GetPosition() const override;
    double GetDuration() const override;

    shared::Result<void> AssignMonitor(const std::string& monitorId) override;
    shared::Result<void> ClearMonitor() override;
    MonitorInfo GetCurrentMonitor() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentMonitor;
    }
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
    bool CaptureFrame(const std::string& pathUtf8) override;
    void GetTelemetry(EngineTelemetry& out) const override;
    bool TryGetTelemetry(EngineTelemetry& out) const override;
    shared::UUID GetSessionId() const override { return m_sessionId; }

    // ─── Per-monitor wallpapers (Phase 5) ────────────────────────────────
    shared::Result<void> SetPerMonitorWallpaper(const std::string& monitorId,
                                                const std::string& path) override;
    void SetSettingsService(
        std::shared_ptr<rex::settings::ISettingsService> settings) override;

private:
    /// Snapshot body; caller must hold m_mutex.
    void GetTelemetryLocked(EngineTelemetry& out) const;
    void Update(double deltaTime);
    void SetState(EngineState state);
    void RenderLoop();
    /// Write the present stage ring to %TEMP%\Paiint\perframe_present.csv
    /// (overwrite). Render-thread only; caller checks REX_PERFRAME_CSV.
    void DumpPresentStages() const;
    /// P16: sleep until the next expected frame (decoder cadence clamped
    /// by maxFps, floored at 4 ms / 250 Hz by the cadence policy so
    /// pause/resume/unload stay responsive).
    void PaceLoop(std::chrono::high_resolution_clock::time_point frameStart,
                  bool isVideoScene);
    shared::Result<void> ReinitializeRenderer();
    shared::Result<void> HandleDeviceLoss();
    void CheckDisplayChanges();
    /// Housekeeping worker thread: runs the display-topology probe off the
    /// render thread (on the render thread it blocked a frame for
    /// 415-1084 ms - measured 2026-10-20).
    void HousekeepingLoop();
    /// Auto-pause/resume while a full-screen app owns the foreground.
    void CheckFullScreenPause();
    /// Suspend sources (P3 / G1+G2): session lock, console display off,
    /// battery saver / critical battery. Edge-posts m_suspendRequest;
    /// the effective pause is fullscreen OR suspend.
    void CheckSuspendSources();
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
    // Shutdown handshake (audit 2026-10-17, #7): the first Shutdown()
    // caller owns the teardown; concurrent callers and the destructor
    // wait on this before touching members. Notified at the end of
    // Shutdown, under m_mutex, when the state flips to Uninitialized.
    std::condition_variable m_shutdownCv;
    // Cross-thread playback-position clock (audit 2026-10-17, #3).
    // Replaces a shared::Stopwatch the render thread read unguarded; see
    // PositionClock.hpp for the publication rules. The base lives inside
    // the clock (folded on every pause/park; zeroed with m_currentPosition).
    PositionClock m_clock;
    // Last position published by Update on the render thread; read by
    // GetPosition/DumpStats under m_mutex — atomic so the unlocked render
    // write is race-free.
    std::atomic<double> m_currentPosition{0.0};

    // ─── Quit-time frame snapshot ───────────────────────────────────────
    // CaptureFrame (command thread) queues a request; RenderLoop (render
    // thread, the renderer's owner) services it and posts the result back.
    std::atomic<bool> m_captureRequested{false};
    std::mutex m_captureMutex;
    std::condition_variable m_captureCv;
    std::string m_capturePath;      // written before the request, read after
    bool m_capturePending = false;  // queued or running (guarded by m_captureMutex)
    bool m_captureDone = false;     // guarded by m_captureMutex
    bool m_captureOk = false;       // guarded by m_captureMutex

    // ─── Render cadence (P2-15): cached decoder fps, render-thread only.
    // Refreshed at most twice/sec (telemetry does ring math); PaceLoop
    // paces at min(maxFps, cached) so the loop never outruns the decoder.
    double m_cachedDecoderFps = 0.0;
    std::chrono::high_resolution_clock::time_point m_cadenceSampledAt{};

    // ─── Telemetry ─────────────────────────────────────────────────────
    // Render-thread writers, command-thread reader (DumpStats): atomic
    // snapshot, never a plain pair (audit 2026-10-17, #9).
    std::atomic<double> m_frameTime{0.0};
    std::atomic<long long> m_frameCount{0};
    std::atomic<uint32_t> m_recoveryCount{0};
    std::atomic<uint32_t> m_deviceResetCount{0};
    int m_lastMonitorCount = 0;
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
    // The housekeeping worker (display-topology probe). The render thread
    // only raises the request flag; the worker does the (slow) probe so a
    // frame is never blocked by it.
    std::thread m_housekeepingThread;
    std::atomic<bool> m_housekeepingStop{true};
    std::atomic<bool> m_housekeepingRequested{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recoveryNeeded{false};
    // True while ReinitializeRenderer has detached the renderer/video
    // backends and is rebuilding them (audit 2026-10-17, #14). During that
    // window the members are null: Play/LoadScene are already rejected
    // because the state stays Error, and Stop/UnloadScene check this flag
    // so neither can flip the state and let a load run against the
    // not-yet-installed backends.
    std::atomic<bool> m_rebuildInProgress{false};
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
    // Inter-present-interval ring. presentFps is a mean over the sampling
    // window, and a mean cannot express jitter: alternating 37.5/43.75 ms and
    // a flat 41.667 ms both average to 24 fps. This ring keeps the distribution
    // so telemetry can publish P50/P95/P99, worst gap and stddev.
    static constexpr size_t kPresentIntervalRingSize = 256;
    std::array<std::atomic<float>, kPresentIntervalRingSize> m_presentIntervalRing{};
    size_t m_presentIntervalIndex{0};
    std::chrono::steady_clock::time_point m_lastPresentAt{};
    // Per-phase work timings for the last frame plus rolling means, so a
    // present-path stall can be attributed rather than guessed at.
    std::atomic<double> m_phaseBeginMs{0.0};   // wake -> BeginFrame returned
    std::atomic<double> m_phaseDrawMs{0.0};    // draw + RenderFrame + Present
    std::atomic<double> m_phaseWorkMs{0.0};    // whole frame work
    std::atomic<double> m_phaseWaitMs{0.0};    // frame-latency waitable block

    // Per-present stage ring (StageTimer.hpp): begin-wait / draw-submit /
    // end-frame(present) split per loop pass for offline deadline analysis.
    // Render-thread owned; dumped to perframe_present.csv when
    // REX_PERFRAME_CSV=1 (see RenderLoop).
    StageRing<PresentStage, 256> m_stageRing;
    uint64_t m_stageSeq = 0;
    std::chrono::steady_clock::time_point m_lastStageDump{};
    // Rolling mean of the frame-latency waitable, used as the GPU-pressure
    // signal. Steady state measures 0.00 ms.
    std::atomic<double> m_phaseWaitMeanMs{0.0};
    std::atomic<uint64_t> m_phaseFrameIndex{0};

    // ─── Pacing servo (Phase B2) ───────────────────────────────────────
    // Render-thread only. Cancels the measured +0.30-0.33 ms per-frame pacing
    // bias that costs 2.00% of rate at 60 fps and 0.97% at 24 fps
    // (docs/bugs/BASELINE-2026-10-15-phaseA.md). Rollback: REX_PACING_SERVO=0.
    PacingServo m_pacingServo{};
    bool m_pacingServoEnabled = true;
    // Previous frame's start, for the achieved-interval measurement that drives
    // the servo. Render-thread only.
    std::chrono::high_resolution_clock::time_point m_pacingPrevFrameStart{};
    bool m_pacingPrevValid = false;
    std::atomic<double> m_pacingCorrMs{0.0};
    std::atomic<double> m_pacingErrorMs{0.0};
    std::atomic<double> m_pacingIntegralMs{0.0};
    std::atomic<bool> m_pacingServoActive{false};

    // Unique-video-present sampler (Phase 3): same folding for
    // effectiveVideoFps (excludes cadence re-presents).
    mutable uint64_t m_uniqueRateBase{0};
    mutable std::chrono::steady_clock::time_point m_uniqueRateTime{};
    mutable double m_effectiveVideoFps{0.0};
    // ─── Wall-cadence video presentation (Phase 3) ────────────────────
    // Render-thread only. Last presented video frame: re-drawn when the
    // scheduler has nothing new due, so presentation holds display
    // cadence while unique-frame rate follows decode throughput.
    VideoFrame m_cachedVideoFrame{};
    // Atomic: cleared from UnloadScene/Stop/ReinitializeRenderer (any
    // thread under m_mutex) while the render loop reads it lock-free.
    // A stale-true costs at most one extra re-present of a still-valid
    // frame; a stale-false skips one re-present. The struct itself is
    // render-thread-only.
    std::atomic<bool> m_hasVideoFrame{false};
    // Bumped under m_mutex whenever the video backend's frames become
    // invalid (UnloadScene / Stop / LoadScene — the recovery reload goes
    // through LoadScene, and the swap itself runs on the render thread, so
    // no in-flight pass exists there). The render thread snapshots the
    // epoch with the scene and re-checks it after BeginFrame before
    // drawing, so a frame taken from Tick() is never drawn across a
    // teardown that may already have released its pool handles
    // (audit 2026-10-17, #6).
    std::atomic<uint64_t> m_videoEpoch{0};
    std::atomic<uint64_t> m_uniqueVideoFrames{0};
    std::atomic<uint64_t> m_repeatedVideoFrames{0};
    // Cached display refresh for the cadence controller (Phase 3):
    // refreshed wherever m_currentMonitor is assigned; Phase 4 owns
    // topology-change refresh completeness.
    std::atomic<int> m_displayHzCached{60};
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
    bool m_fullScreenPaused = false;    /// First sighting of the current full-screen episode (render-thread
    /// only). A shell overlay that flashes fullscreen for a moment must not
    /// freeze the wallpaper — the pause fires only after it persists.
    std::chrono::steady_clock::time_point m_fullScreenSince{};
    /// Suspend channel (P3 / G1+G2: session lock, display off, battery).
    /// Same request protocol as fullscreen (1 = pause, -1 = resume);
    /// the effective pause is fullscreen OR suspend. Render-thread only.
    std::atomic<int> m_suspendRequest{0};
    bool m_suspPaused = false;
    /// True while WE auto-paused (either channel). Set on auto-pause,
    /// cleared on auto-resume AND on any user Pause/Play/Stop — resume
    /// fires only for our own pauses. Touched under m_mutex (user side)
    /// and lock-free on the render thread: atomic.
    std::atomic<bool> m_autoPaused{false};
    /// Occlusion-park decision (vendor-neutral idle). Command thread
    /// Arms/Resets on playback transitions; the render thread Updates per
    /// pass and owns the parked bookkeeping. See OcclusionPolicy.hpp.
    OcclusionPolicy m_occlusion;
    /// Previous pass start for the occlusion dt (pass spacing, clamped).
    /// Default-epoch on the first pass: dt clamps, never a spurious park.
    std::chrono::high_resolution_clock::time_point m_occlusionPrevStart{};
};

} // namespace rex::wallpaper
