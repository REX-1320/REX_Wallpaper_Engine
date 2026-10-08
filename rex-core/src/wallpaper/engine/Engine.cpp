#include "Engine.hpp"
#include "wallpaper/engine/RecoveryBackoff.hpp"
#include "wallpaper/engine/DisplayTopology.hpp"
#include "wallpaper/engine/CadencePolicy.hpp"
#include "wallpaper/playback/CadenceController.hpp"
#include "wallpaper/engine/PowerPolicy.hpp"
#include "platform/windows/DesktopHostFactory.hpp"
#include "platform/windows/MonitorEdid.hpp"
#include <shared/PreciseSleep.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <ShellScalingApi.h>

namespace rex::wallpaper {

namespace {

/// LUID of the adapter that owns the output driving `monitorDeviceName`
/// ("\\.\DISPLAYN", matching MONITORINFOEXW.szDevice / DXGI_OUTPUT_DESC.
/// DeviceName). 0 when not found. Cheap enough for 5s telemetry sampling
/// (a handful of adapter/output GetDesc calls).
uint64_t QueryDisplayAdapterLuid(const std::string& monitorDeviceName) {
    if (monitorDeviceName.empty()) return 0;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, monitorDeviceName.c_str(),
                                           -1, nullptr, 0);
    if (needed <= 0) return 0;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, monitorDeviceName.c_str(), -1,
                        wide.data(), needed);
    wide.resize(static_cast<size_t>(needed) - 1);

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;

    for (UINT a = 0;; ++a) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            if (wide != desc.DeviceName) continue;
            DXGI_ADAPTER_DESC1 adesc{};
            if (FAILED(adapter->GetDesc1(&adesc))) return 0;
            return static_cast<uint64_t>(adesc.AdapterLuid.LowPart) |
                   (static_cast<uint64_t>(adesc.AdapterLuid.HighPart) << 32);
        }
    }
    return 0;
}

} // namespace

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

    // Acquire desktop host. "MultiMonitor" is the explicit opt-in for
    // per-monitor wallpapers; anything else auto-picks the single-monitor
    // strategy (WorkerW → Progman → error).
    std::unique_ptr<rex::platform::windows::IDesktopHost> host;
    if (config.desktopHost == "MultiMonitor") {
        host = rex::platform::windows::DesktopHostFactory::Create("MultiMonitor");
        if (!host) {
            REX_LOG_WARN("Engine",
                "MultiMonitor host unavailable, falling back to auto selection");
            host = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
        }
    } else {
        host = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
    }
    if (!host) {
        REX_LOG_ERROR("Engine", "Failed to create desktop host");
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "No desktop hosting strategy available. "
            "Check the log for details on why WorkerW and "
            "Progman both failed.");
    }

    HWND renderWindow = host->GetRenderWindow();
    int width  = host->GetWidth();
    int height = host->GetHeight();

    // Create Renderer
    m_renderer = std::make_unique<D3D12Renderer>();
    // P0-2: SetForceSdr BEFORE Initialize — the swapchain locks its
    // SDR/HDR format at creation from this flag. Setting it after (as
    // before) left an R16F/PQ swapchain behind a forced-SDR shader.
    m_renderer->SetForceSdr(m_config.forceSdr);
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to initialize renderer: {}", result.Error().message);
        host->Release();
        return result;
    }

    // Video backend (FFmpeg D3D11VA -> D3D12 shared textures). Non-fatal:
    // video scenes simply won't play if this fails.
    m_videoManager = std::make_unique<VideoManager>();
    auto vmResult = m_videoManager->Initialize(m_config, m_renderer->GetDevice().Get());
    if (vmResult.IsError()) {
        REX_LOG_WARN("Engine", "VideoManager init failed: {} (video scenes will not play)",
                     vmResult.Error().message);
        m_videoManager.reset();
    }

    // Success — transfer ownership to member
    m_desktopHost = std::move(host);

    // Multi-monitor mode: one compositor per monitor inside the wallpaper
    // layer + restore persisted per-monitor assignments.
    if (m_videoManager) {
        m_videoManager->SetWallpaperHostWindow(
            m_desktopHost->GetWallpaperLayerWindow());
    }
    SetupMonitorCompositors();

    // Start shell monitor for desktop host recovery
    // The callback runs on a background thread — it only sets a flag.
    // The actual recovery happens on the render thread.
    m_desktopHost->StartMonitor([this]() {
        m_recoveryNeeded.store(true, std::memory_order_release);
    });

    // Cache monitor state for hot-plug detection
    auto monitors = EnumerateMonitors();
    m_lastMonitorCount = static_cast<int>(monitors.size());
    m_lastMonitors = monitors;
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state == EngineState::Uninitialized) {
            // Defensive: clean up anything that may have been allocated during
            // a failed Initialize (e.g. renderer failure after host acquisition).
            if (m_desktopHost) {
                m_desktopHost->Release();
                m_desktopHost.reset();
            }
            m_renderer.reset();
            return shared::Result<void>();
        }

        REX_LOG_INFO("Engine", "Shutting down...");

        SetState(EngineState::ShuttingDown);
        m_desktopHost->StopMonitor();
        m_running = false;
        // NOTE: m_mutex is intentionally released here (end of scope) before
        // joining the render thread. The render thread calls GetPosition() /
        // CheckFullScreenPause() internally which both re-enter m_mutex —
        // joining while holding the lock is a guaranteed deadlock.
    } // <── m_mutex released here

    // The render thread may still be executing its current frame; wait for
    // it to observe m_running==false and exit cleanly.
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    // Render thread is fully stopped — no further concurrent access to
    // m_videoManager / m_renderer / m_desktopHost; no lock needed.
    REX_LOG_INFO("Engine", "Shutdown: video teardown begin");
    if (m_videoManager) {
        m_videoManager->ShutdownMonitorCompositors();
        m_videoManager->Shutdown();
        m_videoManager.reset();
    }
    REX_LOG_INFO("Engine", "Shutdown: video teardown done");
    if (m_audioCapture) {
        m_audioCapture->Stop();
        m_audioCapture.reset();
    }
    m_renderer.reset();
    REX_LOG_INFO("Engine", "Shutdown: renderer destroyed");
    m_adapter.reset();
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

    // The renderer and the video backend are destroyed and rebuilt here on
    // the render thread. m_mutex is held across the swap so no command or
    // telemetry thread can be inside them (GetTelemetry, GetDuration,
    // GetFrameSurface, LoadScene, ...) while they are being replaced.
    // It must NOT be held across the caller: HandleDeviceLoss reloads the
    // scene afterwards and LoadScene/Play take m_mutex themselves.
    std::lock_guard<std::mutex> lock(m_mutex);

    // Destroy old renderer (and the video backend that borrowed its device)
    if (m_videoManager) {
        m_videoManager->ShutdownMonitorCompositors();
        m_videoManager->Shutdown();
        m_videoManager.reset();
    }
    // Phase 3: drop the cadence re-present cache before tearing down —
    // slot handles/fence values below belong to the dying backend.
    m_hasVideoFrame.store(false, std::memory_order_release);
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
    // P0-2: same ordering as Initialize — force-SDR must be visible to the
    // swapchain before it is created, not after.
    m_renderer->SetForceSdr(m_config.forceSdr);
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to reinitialize renderer: {}",
                      result.Error().message);
        return result;
    }

    // Recreate the video backend on the new device.
    m_videoManager = std::make_unique<VideoManager>();
    auto vmResult = m_videoManager->Initialize(m_config, m_renderer->GetDevice().Get());
    if (vmResult.IsError()) {
        REX_LOG_WARN("Engine", "VideoManager re-init failed: {} (video scenes will not play)",
                     vmResult.Error().message);
        m_videoManager.reset();
    }

    // Rebuild per-monitor compositors (device changed) and restore
    // persisted assignments.
    if (m_videoManager) {
        m_videoManager->SetWallpaperHostWindow(
            m_desktopHost->GetWallpaperLayerWindow());
    }
    SetupMonitorCompositors();

    // LoadScene only accepts Initialized/Stopped states; the engine is in
    // Error during recovery, so move to Initialized before the caller
    // (HandleDeviceLoss) reloads the scene.
    SetState(EngineState::Initialized);

    REX_LOG_INFO("Engine", "Renderer reinitialized successfully");
    return shared::Result<void>();
}

// ─── Device-loss recovery ─────────────────────────────────────────────────

shared::Result<void> Engine::HandleDeviceLoss() {
    REX_LOG_INFO("Engine", "Handling device loss...");

    // 1. Save playback state (before any teardown invalidates it).
    const bool wasPlaying = m_playing.load();
    // m_currentScene carries std::strings that the command thread assigns
    // under m_mutex — copying it raw here raced a concurrent LoadScene.
    SceneInfo savedScene;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        savedScene = m_currentScene;
    }
    double savedPosition = 0.0;
    if (m_videoManager && m_videoManager->IsOpen()) {
        savedPosition = m_videoManager->GetPosition();
    }

    // 2. Recreate the renderer (new device/swap chain), reacquire the
    //    desktop host if needed, and rebuild the video backend on the new
    //    device. ReinitializeRenderer tears down and recreates
    //    m_videoManager internally — it must not touch the old one.
    auto result = ReinitializeRenderer();
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Device-loss recovery failed at renderer: {}",
                      result.Error().message);
        return result;
    }

    // 3. Reload the scene (LoadScene is the single entry point: it opens
    //    the video / texture and applies scene settings).
    if (!savedScene.sourcePath.empty()) {
        auto loadResult = LoadScene(savedScene);
        if (loadResult.IsError()) {
            REX_LOG_WARN("Engine", "Reload scene after recovery failed: {}",
                         loadResult.Error().message);
        }
        // Seek to the saved position for video.
        if (savedScene.type == "video" && m_videoManager &&
            m_videoManager->IsOpen()) {
            m_videoManager->Seek(savedPosition);
            // P1-13: re-base the wall clock on the seek target — otherwise
            // the video resumes mid-stream under a ~0 position.
            m_positionBase.store(savedPosition, std::memory_order_release);
            m_stopwatch.Reset();
            m_stopwatch.Start();
        }
    }

    // 4. Restore the playing state.
    if (wasPlaying && !savedScene.sourcePath.empty()) {
        auto playResult = Play();
        if (playResult.IsError()) {
            REX_LOG_WARN("Engine", "Resume after recovery failed: {}",
                         playResult.Error().message);
        }
    }

    REX_LOG_INFO("Engine", "Device-loss recovery complete (resumed at {:.2f}s)",
                 savedPosition);
    return shared::Result<void>();
}


shared::Result<void> Engine::LoadScene(const SceneInfo& scene) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Initialized && m_state != EngineState::Stopped) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "Engine not ready to load scene");
    }

    // Unknown types must fail loudly here — not fall through to an
    // UnsupportedAdapter dark scene that reports success. This is the single
    // choke point for CLI LOAD, playlist, and device-loss recovery.
    if (scene.type != "static" && scene.type != "video" && scene.type != "audio") {
        return shared::Result<void>(shared::ErrorCode::UnsupportedFormat,
            "unsupported scene type '" + scene.type + "' (supported: static, video, audio)");
    }

    REX_LOG_INFO("Engine", "Loading scene: {} (type: {}, path: {})",
                 scene.name.c_str(), scene.type.c_str(), scene.sourcePath.c_str());

    m_scenesLoaded.fetch_add(1, std::memory_order_relaxed);
    m_currentScene = scene;
    m_currentPosition = 0.0;
    m_positionBase.store(0.0, std::memory_order_relaxed);

    // Activate the renderer adapter for this scene type. Every later
    // SetProperty call is gated by what the adapter supports, so
    // unsupported properties fail loudly instead of silently no-oping.
    m_adapter = CreateRendererAdapter(scene.type);

    // Load texture / video based on scene type
    if (scene.type == "static" && m_renderer && !scene.sourcePath.empty()) {
        auto texResult = m_renderer->LoadTexture(scene.sourcePath);
        if (texResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to load texture '{}': {}",
                         scene.sourcePath.c_str(), texResult.Error().message);
        }
        // Enable hot-reload for static images
        m_hotReloadPath = scene.sourcePath;
        {
            std::error_code ec;
            m_hotReloadLastWrite = std::filesystem::last_write_time(m_hotReloadPath, ec);
        }
        // Per-monitor compositors without an override mirror the main scene.
        if (m_videoManager) {
            m_videoManager->SetSharedStaticWallpaper(scene.sourcePath);
        }
    } else if (scene.type == "video" && !scene.sourcePath.empty()) {
        // Clear any leftover static texture so the renderer knows we're in video mode
        if (m_renderer) {
            m_renderer->ClearTexture();
        }
        if (!m_videoManager) {
            REX_LOG_WARN("Engine", "VideoManager not available, video will not play");
        } else {
            auto videoResult = m_videoManager->OpenVideo(scene.sourcePath);
            if (videoResult.IsError()) {
                REX_LOG_WARN("Engine", "Failed to open video '{}': {}",
                             scene.sourcePath.c_str(), videoResult.Error().message);
                // A failed open must not present stale (or uninitialized)
                // pool slots as if the new scene were playing — that is the
                // desktop white-noise symptom. Drop all GPU frames so the
                // compositor falls back to clear color, and report failure
                // so LOAD -> ok:false and callers skip PLAY.
                m_videoManager->ClearVideoFrames();
                if (m_renderer) {
                    m_renderer->ClearVideoFrame();
                }
                // Don't leave the failed scene as "current": Play() must
                // refuse ("No scene loaded") instead of presenting nothing.
                m_currentScene = SceneInfo{};
                m_playing = false;
                return videoResult;
            } else {
                // Apply scene-level playback settings immediately (all four
                // knobs in one path — P1-7).
                ApplyPlaybackSettings();
                // Adaptive resolution: pick the stream that best fits the
                // target monitor. A stream switch may rebuild the pool with
                // new NT handles; the ClearVideoFrame() below drops the
                // renderer's stale slot cache.
                if (m_desktopHost) {
                    m_videoManager->SetTargetResolution(m_desktopHost->GetWidth(),
                                                        m_desktopHost->GetHeight());
                }
            }
        }
    } else if (scene.type == "audio") {
        // Audio visualisation: capture loopback audio for spectrum.
        if (!m_audioCapture) {
            m_audioCapture = std::make_shared<AudioCapture>();
        }
        if (!m_audioCapture->IsRunning()) {
            m_audioCapture->Start();
        }
        m_hotReloadPath.clear();
    } else {
        // Non-static scene: disable hot-reload
        m_hotReloadPath.clear();
    }

    // Push scene-level video adjustments to the renderer (and compositors).
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
        m_videoManager->ApplyFillMode(m_currentScene.fillMode);
    }

    // Push the transform (rotation/flip) to the renderer and compositors.
    ApplyTransform();

    // Drop any GPU frames from a previous video scene.
    if (m_renderer) {
        m_renderer->ClearVideoFrame();
    }

    SetState(EngineState::Stopped);
    return shared::Result<void>();
}

shared::Result<void> Engine::UnloadScene() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Unloading scene");

    m_scenesReleased.fetch_add(1, std::memory_order_relaxed);

    if (m_videoManager) {
        m_videoManager->Close();
        m_videoManager->ClearVideoFrames();
    }
    // Phase 3: no scene, no cached re-present.
    m_hasVideoFrame.store(false, std::memory_order_release);

    if (m_audioCapture) {
        m_audioCapture->Stop();
    }

    if (m_renderer) {
        m_renderer->ClearTexture();
        m_renderer->ClearVideoFrame();
    }

    m_adapter.reset();
    m_playing = false;
    m_currentScene = SceneInfo{};
    m_currentPosition = 0.0;
    m_positionBase.store(0.0, std::memory_order_relaxed);

    SetState(EngineState::Initialized);
    return shared::Result<void>();
}

shared::Result<void> Engine::Play() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_currentScene.sourcePath.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "No scene loaded");
    }

    REX_LOG_INFO("Engine", "Play requested");

    if (m_videoManager && m_currentScene.type == "video") {
        m_videoManager->Play();
    }

    m_playing = true;
    SetState(EngineState::Running);
    m_stopwatch.Start();

    // P16: arm the first present after (re)starting playback.
    m_frameDirty.store(true, std::memory_order_release);
    // User-driven play overrides any auto-pause bookkeeping: a later flag
    // clear must not "resume" (re-pause state is re-evaluated anyway).
    m_autoPaused.store(false, std::memory_order_release);
    return shared::Result<void>();
}

shared::Result<void> Engine::Pause() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Running) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "Not playing");
    }

    REX_LOG_INFO("Engine", "Pause requested");

    if (m_videoManager) {
        m_videoManager->Pause();
    }

    m_playing = false;
    SetState(EngineState::Paused);
    // Fold the elapsed time into the base before stopping: Start() re-stamps
    // from zero, so without this every pause/resume rewound the reported
    // position while the decoder kept playing from its own PTS. Scaled by
    // the playback rate (Phase 4) so pause at 2x banks 2x time.
    m_positionBase.store(m_positionBase.load(std::memory_order_relaxed) +
                         m_stopwatch.ElapsedSeconds() *
                             m_speed.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    m_stopwatch.Reset();
    // User-driven pause: a later auto-resume must not override it.
    m_autoPaused.store(false, std::memory_order_release);

    return shared::Result<void>();
}

shared::Result<void> Engine::Stop() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Stop requested");

    if (m_videoManager) {
        m_videoManager->Stop();
    }

    m_playing = false;
    m_currentPosition = 0.0;
    m_positionBase.store(0.0, std::memory_order_relaxed);
    // Phase 3: stopped video must not keep re-presenting its last frame
    // at cadence (a stale wallpaper behind the clear color).
    m_hasVideoFrame.store(false, std::memory_order_release);
    SetState(EngineState::Stopped);
    m_stopwatch.Reset();
    // User-driven stop: a later auto-resume must not override it.
    m_autoPaused.store(false, std::memory_order_relaxed);

    return shared::Result<void>();
}

double Engine::GetPosition() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_playing) {
        return m_currentPosition;
    }
    double pos = m_positionBase.load(std::memory_order_relaxed) +
                 m_stopwatch.ElapsedSeconds() *
                     m_speed.load(std::memory_order_relaxed);
    // P1-13: the wall clock drifts from decoder PTS (speed changes, loop
    // wraps, recovery seeks). Reconcile against duration: wrap when looping,
    // clamp otherwise. No duration (static/unopened) → wall clock as-is.
    const double dur = (m_videoManager && m_videoManager->IsOpen())
                           ? m_videoManager->GetDuration()
                           : 0.0;
    if (dur > 0.0) {
        if (m_currentScene.loop) {
            pos = std::fmod(pos, dur);
        } else if (pos > dur) {
            pos = dur;
        }
    }
    return pos;
}

double Engine::GetDuration() const {
    // m_videoManager is destroyed and rebuilt by the render thread during
    // recovery; every reader must hold m_mutex or it can dereference a
    // freed object (same rule as GetFrameSurface / GetFrameSlotHandle).
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_videoManager && m_videoManager->IsOpen()) {
        return m_videoManager->GetDuration();
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
            m_displayHzCached.store(
                SelectDisplayHz(monitors, m_currentMonitor.id, 60),
                std::memory_order_release);
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
    m_displayHzCached.store(60, std::memory_order_release);

    REX_LOG_WARN("Engine", "Monitor '{}' not found, using fallback {}x{}",
                 monitorId.c_str(), m_currentMonitor.width, m_currentMonitor.height);

    return shared::Result<void>();
}

shared::Result<void> Engine::ClearMonitor() {
    std::lock_guard<std::mutex> lock(m_mutex);
    REX_LOG_INFO("Engine", "Clearing monitor");
    m_currentMonitor = MonitorInfo{};
    m_displayHzCached.store(60, std::memory_order_release);
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

            // DPI scale — real per-monitor effective DPI (P0-2 / G3).
            // Falls back to 1.0 when the query fails (Stale Shcore, remote
            // session); coordinates stay in EnumDisplayMonitors space.
            UINT dpiX = 96, dpiY = 96;
            if (FAILED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))
                || dpiX == 0) {
                dpiX = 96;
            }
            info.dpiScale = static_cast<float>(dpiX) / 96.0f;

            // Refresh rate (Phase 1): current-mode frequency for the
            // CadenceController. 0 when unknowable (remote session);
            // values <= 1 are driver placeholders, not real rates.
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
                && dm.dmDisplayFrequency > 1) {
                info.refreshHz = static_cast<int>(dm.dmDisplayFrequency);
            }

            // Stable identity across disconnects (EDID descriptor hash).
            info.edidHash = rex::platform::windows::GetMonitorEdidHash(hMon);

            vec->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    return monitors;
}

shared::Result<void> Engine::SetFillMode(FillMode mode) {
    if (mode == FillMode::Tile) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Tile fill mode is not implemented");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.fillMode = mode;
    if (m_renderer) {
        m_renderer->SetFillMode(mode);
    }
    if (m_videoManager) {
        m_videoManager->ApplyFillMode(mode);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Fill mode set to {}", static_cast<int>(mode));
    return shared::Result<void>();
}

shared::Result<void> Engine::SetBrightness(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.brightness = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Brightness set to {:.2f}", m_currentScene.brightness);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetContrast(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.contrast = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Contrast set to {:.2f}", m_currentScene.contrast);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSaturation(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.saturation = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Saturation set to {:.2f}", m_currentScene.saturation);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetVolume(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.volume = std::clamp(value, 0.0f, 1.0f);
    if (m_videoManager) {
        m_videoManager->SetVolume(m_currentScene.volume);
    }
    REX_LOG_DEBUG("Engine", "Volume set to {:.2f}", m_currentScene.volume);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetMute(bool mute) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = mute;
    m_currentScene.muted = mute;
    if (m_videoManager) {
        m_videoManager->SetMute(mute);
    }
    REX_LOG_DEBUG("Engine", "Mute set to {}", mute ? "true" : "false");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetLoop(bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loop = loop;
    m_currentScene.loop = loop;
    if (m_videoManager) {
        m_videoManager->SetLoop(loop);
    }
    REX_LOG_DEBUG("Engine", "Loop set to {}", loop ? "true" : "false");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speed = std::clamp(speed, 0.25f, 4.0f);
    m_currentScene.speed = m_speed.load();
    if (m_videoManager) {
        m_videoManager->SetSpeed(m_speed.load());
    }
    REX_LOG_DEBUG("Engine", "Speed set to {:.2f}", m_speed.load());
    return shared::Result<void>();
}

// P1-7: single path restoring ALL playback knobs (loop/speed/volume/mute)
// from the current scene. LoadScene AND device-loss recovery funnel through
// here — previously only loop+speed were pushed, so every LOAD and every
// recovery dropped volume and unmuted.
void Engine::ApplyPlaybackSettings() {
    if (!m_videoManager) return;
    m_videoManager->SetLoop(m_currentScene.loop);
    m_videoManager->SetSpeed(std::clamp(m_currentScene.speed, 0.25f, 4.0f));
    m_videoManager->SetVolume(m_currentScene.volume);
    m_videoManager->SetMute(m_currentScene.muted);
    REX_LOG_DEBUG("Engine", "Playback settings pushed: loop={} speed={:.2f} "
                            "volume={:.2f} mute={}",
                  m_currentScene.loop, m_currentScene.speed,
                  m_currentScene.volume, m_currentScene.muted);
}

// ─── Renderer Adapter ────────────────────────────────────────────────────

shared::Result<void> Engine::SetProperty(const std::string& property, const std::string& value) {
    bool supported = false;
    std::string rendererType;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_adapter) {
            return shared::Result<void>(shared::ErrorCode::InvalidState,
                "No scene loaded; no renderer adapter active");
        }
        supported = m_adapter->Supports(property);
        rendererType = std::string(m_adapter->GetType());
    }

    if (!supported) {
        return shared::Result<void>(shared::ErrorCode::UnsupportedFormat,
            "Property '" + property + "' is not supported by the " + rendererType + " renderer");
    }

    if (property == "fillMode") {
        auto mode = ParseFillMode(value);
        if (!mode) {
            return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                "Unknown fill mode: " + value);
        }
        return SetFillMode(*mode);
    }
    if (property == "volume") {
        return SetVolume(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "mute") {
        return SetMute(value == "true" || value == "1");
    }
    if (property == "speed") {
        return SetSpeed(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "loop") {
        return SetLoop(value == "true" || value == "1");
    }
    if (property == "rotation") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sceneRotation = static_cast<float>(std::atof(value.c_str()));
        ApplyTransform();
        return shared::Result<void>();
    }
    if (property == "flipH" || property == "flipV") {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool on = (value == "true" || value == "1");
        if (property == "flipH") m_sceneFlipH = on;
        else                     m_sceneFlipV = on;
        ApplyTransform();
        return shared::Result<void>();
    }

    return shared::Result<void>(shared::ErrorCode::InvalidArgument,
        "Unknown property: " + property);
}

std::vector<std::string> Engine::GetSupportedProperties() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_adapter) return {};
    return m_adapter->GetSupportedProperties();
}

std::string Engine::GetRendererType() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_adapter) return "none";
    return std::string(m_adapter->GetType());
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

IWallpaperEngine::FrameSurface Engine::GetFrameSurface() const {
    FrameSurface s;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_videoManager || !m_videoManager->IsOpen()) {
        return s;
    }
    const auto last = m_videoManager->GetLastFrame();
    s.available = true;
    s.fenceHandle = reinterpret_cast<uint64_t>(m_videoManager->GetFenceHandle());
    s.adapterLuid = m_videoManager->GetAdapterLuid();
    s.slotCount = m_videoManager->GetSlotCount();
    s.currentSlot = last.slot;
    s.currentFenceValue = last.fenceValue;
    s.width = last.width;
    s.height = last.height;
    s.frameIndex = last.frameIndex;
    s.generation = m_videoManager->GetSurfaceGeneration();
    return s;
}

uint64_t Engine::GetFrameSlotHandle(uint32_t slot) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_videoManager || !m_videoManager->IsOpen()) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(m_videoManager->GetSlotHandle(slot));
}

void Engine::GetTelemetry(EngineTelemetry& out) const {
    // Locked: the 5 s telemetry sampler runs on its own thread while the
    // render thread may be tearing these objects down in ReinitializeRenderer.
    std::lock_guard<std::mutex> lock(m_mutex);
    out = EngineTelemetry{};

    if (m_videoManager) {
        VideoPlaybackTelemetry vt = m_videoManager->GetTelemetry();
        out.framesDecoded  = vt.framesDecoded;
        out.framesConsumed = vt.framesConsumed;
        out.framesDropped  = vt.framesDropped;
        out.decoderFps     = vt.decoderFps;
        out.renderFps      = vt.renderFps;
        out.sourceFps      = vt.sourceFps;
        out.convertFps     = vt.convertFps;
        out.schedulerDrops = vt.schedulerDrops;
        out.queueDepth     = vt.queueDepth;
        out.pacingMs       = vt.pacingMs;
        out.playbackSpeed  = vt.playbackSpeed;
        out.repeatedFrames =
            m_repeatedVideoFrames.load(std::memory_order_relaxed);
        // repeatedFrames stays 0 until Phase 3 starts re-presenting.
        out.ringSlotCount  = vt.ringSlotCount;
        out.ringBytes      = vt.ringBytes;
        out.decodeMsP50    = vt.decodeMsP50;
        out.decodeMsP95    = vt.decodeMsP95;
        out.decodeMsP99    = vt.decodeMsP99;
        out.vpMsP50        = vt.vpMsP50;
        out.vpMsP95        = vt.vpMsP95;
        out.vpMsP99        = vt.vpMsP99;
    }    if (m_renderer) {
        out.textureBytes    = m_renderer->GetTextureBytes();
        out.uploadBytes     = m_renderer->GetUploadBytes();
        out.textureRecreations = m_renderer->GetTextureRecreations();
        out.uploads         = m_renderer->GetUploadCount();
        out.allocatorsPerFrame = m_renderer->GetAllocatorsPerUpload();
    }

    out.videoWidth   = 0;
    out.videoHeight  = 0;
    if (m_videoManager && m_currentScene.type == "video") {
        out.videoWidth  = m_videoManager->GetWidth();
        out.videoHeight = m_videoManager->GetHeight();
    }

    // Adaptive resolution: size of the stream actually being decoded.
    if (m_videoManager && m_videoManager->IsOpen()) {
        out.decodeWidth  = m_videoManager->GetWidth();
        out.decodeHeight = m_videoManager->GetHeight();
    }
    if (m_renderer) {
        out.hdrActive      = m_renderer->IsHDRActive();
        out.displayMaxNits = m_renderer->GetDisplayMaxNits();
    }

    // Adapter topology: decoder (D3D11) vs renderer (D3D12) vs display.
    if (m_videoManager) {
        out.decoderAdapterLuid  = m_videoManager->GetAdapterLuid();
        out.rendererAdapterLuid = m_videoManager->GetD3D12AdapterLuid();
        out.decoderRendererSameAdapter = m_videoManager->IsDecoderRendererSameAdapter();
        const uint64_t displayLuid = QueryDisplayAdapterLuid(m_currentMonitor.id);
        out.rendererDisplaySameAdapter =
            displayLuid != 0 && out.rendererAdapterLuid != 0 &&
            displayLuid == out.rendererAdapterLuid;
    }

    // GPU memory on the renderer's adapter (LOCAL = dedicated VRAM,
    // NON_LOCAL = shared/system-memory segment).
    // NOTE: ID3D12Device does NOT implement IDXGIDevice — resolve the
    // adapter via GetAdapterLuid() + EnumAdapterByLuid instead.
    if (m_renderer && m_videoManager) {
        const uint64_t rendererLuid = m_videoManager->GetD3D12AdapterLuid();
        HRESULT hrFactory = E_FAIL, hrEnum = E_FAIL, hrAs = E_FAIL,
                hrLocal = E_FAIL, hrNonLocal = E_FAIL;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
        if (rendererLuid != 0) {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            hrFactory = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hrFactory)) {
                LUID luid{};
                luid.LowPart  = static_cast<DWORD>(rendererLuid & 0xFFFFFFFFull);
                luid.HighPart = static_cast<LONG>(rendererLuid >> 32);
                Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                hrEnum = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
                if (SUCCEEDED(hrEnum)) {
                    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
                    hrAs = adapter.As(&adapter3);
                    if (SUCCEEDED(hrAs)) {
                        hrLocal    = adapter3->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
                        hrNonLocal = adapter3->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
                    }
                }
            }
        }

        // One-shot diagnostic: budget==0 means the chain broke somewhere,
        // which would silently blind every downstream comparison.
        static std::atomic<bool> s_gpuMemWarned{false};
        if (!s_gpuMemWarned.exchange(true) &&
            (rendererLuid == 0 || FAILED(hrFactory) || FAILED(hrEnum) ||
             FAILED(hrAs) || FAILED(hrLocal) || FAILED(hrNonLocal))) {
            REX_LOG_WARN("Engine",
                         "GPU memory query incomplete: luid={:016x} "
                         "factory={:08x} enum={:08x} as3={:08x} "
                         "local={:08x} nonlocal={:08x}",
                         static_cast<uint64_t>(rendererLuid),
                         static_cast<uint32_t>(hrFactory),
                         static_cast<uint32_t>(hrEnum),
                         static_cast<uint32_t>(hrAs),
                         static_cast<uint32_t>(hrLocal),
                         static_cast<uint32_t>(hrNonLocal));
        }
        if (SUCCEEDED(hrLocal)) {
            out.gpuDedicatedBytes = local.CurrentUsage;
            out.gpuBudgetBytes    = local.Budget;
        }
        if (SUCCEEDED(hrNonLocal)) {
            out.gpuSharedBytes = nonlocal.CurrentUsage;
        }
    }

    out.scenesLoaded   = m_scenesLoaded.load(std::memory_order_relaxed);
    out.scenesReleased = m_scenesReleased.load(std::memory_order_relaxed);

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
        out.privateBytesMB = static_cast<double>(pmc.PrivateUsage)
                           / (1024.0 * 1024.0);
    }

    out.presentCount = m_presentCount.load(std::memory_order_relaxed);
    // Present-rate sampler: fold count deltas into presentFps, refreshed
    // at most every 0.5 s so back-to-back GET_STATUS calls stay stable.
    {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(
            now - m_presentRateTime).count();
        if (dt >= 0.5) {
            if (m_presentRateTime.time_since_epoch().count() != 0) {
                m_presentFps = (static_cast<double>(out.presentCount)
                                - static_cast<double>(m_presentRateBase)) / dt;
            }
            m_presentRateBase = out.presentCount;
            m_presentRateTime = now;
        }
        out.presentFps = m_presentFps;
    }
    // Unique-video-present sampler (Phase 3): same folding over
    // m_uniqueVideoFrames, so effectiveVideoFps excludes cadence
    // re-presents. Shares the present-rate window.
    {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(
            now - m_uniqueRateTime).count();
        if (dt >= 0.5) {
            if (m_uniqueRateTime.time_since_epoch().count() != 0) {
                const uint64_t uniques =
                    m_uniqueVideoFrames.load(std::memory_order_relaxed);
                m_effectiveVideoFps =
                    (static_cast<double>(uniques)
                     - static_cast<double>(m_uniqueRateBase)) / dt;
            }
            m_uniqueRateBase =
                m_uniqueVideoFrames.load(std::memory_order_relaxed);
            m_uniqueRateTime = now;
        }
        out.effectiveVideoFps = m_effectiveVideoFps;
    }

    // Display refresh rate for the current monitor (Phase 1): fresh
    // enumeration matched by monitor id; 0 when unknown.
    out.displayHz = 0;
    if (!m_currentMonitor.id.empty()) {
        for (const auto& info : EnumerateMonitors()) {
            if (info.id == m_currentMonitor.id) {
                out.displayHz = info.refreshHz;
                break;
            }
        }
    }
}

void Engine::DumpStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "=== Engine Stats ===");
    REX_LOG_INFO("Engine", "  State:           {}", static_cast<int>(m_state.load(std::memory_order_acquire)));
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

    if (m_videoManager) {
        auto tel = m_videoManager->GetTelemetry();
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
        // Read the stopwatch directly — this method runs on the render
        // thread which owns m_stopwatch and m_currentPosition. Calling
        // GetPosition() here would try to acquire m_mutex from the render
        // thread, creating a deadlock with Shutdown() holding m_mutex and
        // waiting for the render thread to exit.
        m_currentPosition = m_positionBase.load(std::memory_order_relaxed) +
                            m_stopwatch.ElapsedSeconds();

        if (m_frameCount % 60 == 0) {
            REX_LOG_DEBUG("Engine", "Frame {}: {:.2f} s, {:.2f} ms",
                          m_frameCount, m_currentPosition, deltaTime);
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

    bool changed = MonitorTopologyChanged(m_lastMonitors, monitors);

    // P0-2: re-evaluate HDR on EVERY topology check, not just on geometry
    // changes — a same-resolution OS HDR toggle moves no pixels, so the
    // predicate above never fires for it. RefreshHdrOutput is idempotent
    // (the swapchain bails before any DXGI churn when nothing changed).
    if (m_renderer) {
        m_renderer->RefreshHdrOutput();
    }

    if (!changed) return;

    REX_LOG_INFO("Engine", "Display topology changed: {} monitors (was {})",
                 count, m_lastMonitorCount);

    m_lastMonitorCount = count;
    m_lastPrimaryWidth  = primaryW;
    m_lastPrimaryHeight = primaryH;
    m_lastMonitors = monitors;

    // Phase 4: a refresh-rate change flips the predicate above — refresh
    // the cadence cache from the same enumeration so presentation follows
    // without restarting the wallpaper.
    if (!m_currentMonitor.id.empty()) {
        m_displayHzCached.store(
            SelectDisplayHz(monitors, m_currentMonitor.id,
                            m_displayHzCached.load(std::memory_order_acquire)),
            std::memory_order_release);
    }

    // Multi-monitor mode: reconcile compositors with the new topology
    // (add connected, remove disconnected, update moved/resized).
    if (IsMultiMonitorMode() && m_videoManager) {
        auto active = m_videoManager->GetActiveMonitors();

        for (const auto& a : active) {
            bool found = false;
            for (const auto& m : monitors) {
                if (m.id != a.id) continue;
                found = true;
                if (m.width != a.width || m.height != a.height ||
                    m.x != a.x || m.y != a.y) {
                    m_videoManager->UpdateMonitor(m);
                }
                break;
            }
            if (!found) {
                auto rm = m_videoManager->RemoveMonitor(a.id);
                if (rm.IsError()) {
                    REX_LOG_WARN("Engine", "RemoveMonitor failed: {}",
                                 rm.Error().message);
                }
            }
        }

        for (const auto& m : monitors) {
            bool found = false;
            for (const auto& a : active) {
                if (a.id == m.id) { found = true; break; }
            }
            if (!found) {
                auto am = m_videoManager->AddMonitor(m);
                if (am.IsError()) {
                    REX_LOG_WARN("Engine", "AddMonitor failed: {}",
                                 am.Error().message);
                }
            }
        }
    }

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
    m_state.store(state, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "State changed to {}", static_cast<int>(state));
}

void Engine::ApplyTransform() {
    if (m_renderer) {
        m_renderer->SetRotation(m_sceneRotation);
        m_renderer->SetFlip(m_sceneFlipH, m_sceneFlipV);
    }
    if (m_videoManager) {
        m_videoManager->ApplyTransform(m_sceneRotation, m_sceneFlipH,
                                       m_sceneFlipV);
    }
}

void Engine::CheckFullScreenPause() {
    if (!m_config.pauseOnFullscreen) return;

    // A foreground window counts as full-screen only when it covers
    // (essentially) every pixel of its monitor. The old >=95% area test
    // paused the wallpaper behind ANY maximized window: on a 3840x2160
    // screen with a 48 px taskbar a maximized window covers 3840x2112 =
    // 97.8%, so the wallpaper froze ~2 s after every Play. A maximized
    // window always leaves the taskbar/appbar strip uncovered, which this
    // test now requires to be (near-)zero.
    bool fullScreen = false;
    HWND fg = GetForegroundWindow();
    if (fg && fg != GetDesktopWindow() && IsWindowVisible(fg) && !IsIconic(fg)) {
        wchar_t fgClass[64] = {};
        GetClassNameW(fg, fgClass, 64);
        const bool isShellChrome =
            lstrcmpW(fgClass, L"Progman") == 0 ||
            lstrcmpW(fgClass, L"WorkerW") == 0 ||
            lstrcmpW(fgClass, L"SHELLDLL_DefView") == 0 ||
            lstrcmpW(fgClass, L"Shell_TrayWnd") == 0;
        if (!isShellChrome) {
            RECT fgRect;
            if (GetWindowRect(fg, &fgRect) && fgRect.right > fgRect.left &&
                fgRect.bottom > fgRect.top) {
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST),
                                    &mi)) {
                    RECT inter;
                    IntersectRect(&inter, &fgRect, &mi.rcMonitor);
                    const int iw = inter.right - inter.left;
                    const int ih = inter.bottom - inter.top;
                    const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
                    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
                    // 2 px tolerance for borderless windows that sit 1 px
                    // inside the edges; a taskbar/appbar strip is tens of
                    // pixels and always fails this test.
                    fullScreen = (mw - iw <= 2 && mh - ih <= 2);
                    if (fullScreen) {
                        char cls[64] = {};
                        WideCharToMultiByte(CP_UTF8, 0, fgClass, -1, cls,
                                            sizeof(cls), nullptr, nullptr);
                        REX_LOG_INFO("Engine",
                                     "Full-screen window: class={} rect={}x{} "
                                     "(monitor {}x{})",
                                     cls, iw, ih, mw, mh);
                    }
                }
            }
        }
    }

    // IMPORTANT: Do NOT call Pause()/Play() directly here. Both methods
    // acquire m_mutex, and this function runs on the render thread. If
    // Shutdown() is concurrently holding m_mutex and waiting for the render
    // thread to exit, calling Pause()/Play() here would deadlock.
    // Instead, post an atomic signal that the render loop services before
    // the next GPU submit — fully lock-free on the render thread.
    //
    // A shell overlay that flashes fullscreen for a moment (observed: a
    // XamlWindow covering the monitor for ~1 s) must not freeze the
    // wallpaper: the pause fires only after the state persists across checks.
    if (fullScreen && !m_fullScreenPaused) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (m_fullScreenSince.time_since_epoch().count() == 0) {
            m_fullScreenSince = now;
        }
        if (m_state == EngineState::Running &&
            now - m_fullScreenSince >= milliseconds(1500)) {
            m_fullScreenPaused = true;
            REX_LOG_INFO("Engine", "Full-screen app detected — requesting wallpaper pause");
            m_fullScreenRequest.store(1, std::memory_order_release);
        }
    } else if (!fullScreen) {
        m_fullScreenSince = {};
        if (m_fullScreenPaused) {
            m_fullScreenPaused = false;
            REX_LOG_INFO("Engine", "Full-screen app closed — requesting wallpaper resume");
            m_fullScreenRequest.store(-1, std::memory_order_release);
        }
    }
}

// ─── Suspend sources (P3 / G1+G2) ───────────────────────────────────────
// Session lock, console display off (OS notifications on the render window,
// see WorkerWManager::WndProc) and battery state (polled — no window
// needed). Any active source suspends expensive work; resume fires only
// when ALL are clear. Same lock-free request protocol as fullscreen.

void Engine::CheckSuspendSources() {
    bool locked = false;
    bool displayOff = false;
    if (m_desktopHost) {
        locked = m_desktopHost->IsSessionLocked();
        displayOff = m_desktopHost->IsDisplayOff();
    }

    bool powerSaving = false;
    SYSTEM_POWER_STATUS ps = {};
    if (GetSystemPowerStatus(&ps)) {
        // 255 = unknown: fail open (treat AC as online, percent as unknown).
        const bool acOnline = ps.ACLineStatus != 0;
        const int pct = ps.BatteryLifePercent > 100
                            ? -1
                            : static_cast<int>(ps.BatteryLifePercent);
        powerSaving = ShouldPauseForPower(acOnline,
                                          (ps.SystemStatusFlag & 1) != 0, pct);
    }

    const bool suspended = locked || displayOff || powerSaving;
    if (suspended && !m_suspPaused) {
        m_suspPaused = true;
        REX_LOG_INFO("Engine", "Suspend source active (session-locked={}, "
                               "display-off={}, power-saving={}) — requesting pause",
                     locked, displayOff, powerSaving);
        m_suspendRequest.store(1, std::memory_order_release);
    } else if (!suspended && m_suspPaused) {
        m_suspPaused = false;
        REX_LOG_INFO("Engine", "Suspend sources clear — requesting resume");
        m_suspendRequest.store(-1, std::memory_order_release);
    }
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

        // ── Auto-pause behind full-screen apps (throttled) ──────────────
        if (m_frameCount % 30 == 0) {
            CheckFullScreenPause();
            CheckSuspendSources();
        }

        // ── Service pause/resume requests (lock-free) ──────────────────
        // Fullscreen and suspend sources post independently; the effective
        // pause is their OR. Resume fires only for OUR pauses (m_autoPaused)
        // — never a user-initiated pause. We service the requests here, at
        // the top of the frame, before any GPU work begins.
        if (const int fsReq = m_fullScreenRequest.exchange(0, std::memory_order_acq_rel)) {
            m_fullScreenPaused = (fsReq == 1);
        }
        if (const int suspReq = m_suspendRequest.exchange(0, std::memory_order_acq_rel)) {
            m_suspPaused = (suspReq == 1);
        }
        const bool wantPaused = m_fullScreenPaused || m_suspPaused;
        if (wantPaused && !m_autoPaused.load(std::memory_order_acquire) &&
            m_state == EngineState::Running) {
                // Pause: stop video + mark engine paused (no m_mutex needed;
                // render thread exclusively owns these during normal operation)
                if (m_videoManager) m_videoManager->Pause();
                m_playing = false;
                // Fold first (same reason as Pause()): the stopwatch restarts
                // from zero, so the position must be banked or the next
                // resume rewinds it while the decoder kept its own PTS.
                // Rate-scaled like Pause() (Phase 4).
                m_positionBase.store(
                    m_positionBase.load(std::memory_order_relaxed) +
                        m_stopwatch.ElapsedSeconds() *
                            m_speed.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m_stopwatch.Reset();
                SetState(EngineState::Paused);
                m_autoPaused.store(true, std::memory_order_release);
                REX_LOG_INFO("Engine", "Auto-pause applied (fullscreen={}, suspend={})",
                             m_fullScreenPaused, m_suspPaused);
            } else if (!wantPaused && m_autoPaused.load(std::memory_order_acquire) &&
                       m_state == EngineState::Paused) {
                // Resume: restart video + mark engine running
                if (m_videoManager) m_videoManager->Play();
                m_playing = true;
                m_stopwatch.Start();
                SetState(EngineState::Running);
                m_autoPaused.store(false, std::memory_order_release);
                REX_LOG_INFO("Engine", "Auto-resume applied");
            }


        // ── Periodically check for display topology changes ─────────────
        if (m_frameCount % 120 == 0) {
            CheckDisplayChanges();
        }

        // ── Desktop host / shell restart + device-loss recovery ────────
        // Both sources funnel into ONE gated attempt. Previously each had
        // its own block that `continue`d without pacing and without
        // re-arming the flag: a host that could not be reacquired (Explorer
        // still restarting) spun this thread at 100% CPU, and because the
        // frame counter stopped advancing, ran the %30/%120 housekeeping on
        // every pass. A single failed attempt also used to consume the
        // request and never retry, bricking the wallpaper for the session.
        const bool hostRecoveryRequested = m_recoveryNeeded.exchange(false);
        const bool deviceLost = m_renderer && m_renderer->IsDeviceRemoved();
        if (hostRecoveryRequested || deviceLost) {
            const auto now = high_resolution_clock::now();
            if (now >= m_nextRecoveryAttempt) {
                REX_LOG_INFO("Engine", "Recovery requested ({}), reinitializing...",
                             deviceLost ? "device removed" : "desktop host");
                SetState(EngineState::Error);
                m_recoveryCount++;
                if (deviceLost) m_deviceResetCount++;

                auto recoveryResult = HandleDeviceLoss();
                if (recoveryResult.IsSuccess()) {
                    m_frameCount = 0;
                    m_recoveryDelayMs = 0;
                    m_nextRecoveryAttempt = now;
                } else {
                    // Re-arm and back off — the host normally comes back, so
                    // keep retrying instead of giving up on the wallpaper.
                    m_recoveryDelayMs = NextRecoveryDelayMs(m_recoveryDelayMs);
                    m_nextRecoveryAttempt = now + milliseconds(m_recoveryDelayMs);
                    m_recoveryNeeded.store(true, std::memory_order_release);
                    REX_LOG_ERROR("Engine",
                                  "Recovery failed: {} — retrying in {} ms",
                                  recoveryResult.Error().message,
                                  m_recoveryDelayMs);
                }
            }
            // Always pace: this thread must never busy-loop, whatever the
            // recovery outcome was.
            m_frameCount++;
            PaceLoop(frameStart, false);
            continue;
        }

        if (m_renderer && m_renderer->IsInitialized()) {
            // ── P16 dirty-frame gate ────────────────────────────────────
            // Poll the scheduler EVERY wake (lock-free SPSC, pointer stays
            // valid until the next Tick), but only Begin/Draw/Present when
            // content actually changed. Without this, the loop re-rendered
            // identical frames at ~refresh rate — burning 36-55% GPU when
            // the decoder ran slower than the display.
            const VideoFrame* pending = nullptr;
            // Snapshot the command-thread-owned scene/capture state under a
            // short lock. LoadScene/UnloadScene/Stop assign m_currentScene,
            // m_hotReloadPath and m_audioCapture under m_mutex while this
            // thread used to read them raw: a torn std::string read fed
            // LoadTexture a half-updated buffer, and a reset unique_ptr was a
            // use-after-free of a live AudioCapture. Everything below works
            // on the local copies.
            std::string renderSceneType;
            std::string renderHotReloadPath;
            std::filesystem::file_time_type renderHotReloadLastWrite{};
            std::shared_ptr<AudioCapture> renderAudioCapture;
            {
                std::lock_guard<std::mutex> snapshotLock(m_mutex);
                renderSceneType = m_currentScene.type;
                renderHotReloadPath = m_hotReloadPath;
                renderHotReloadLastWrite = m_hotReloadLastWrite;
                renderAudioCapture = m_audioCapture;
            }
            if (m_state == EngineState::Running && m_videoManager
                && m_videoManager->IsOpen()
                && renderSceneType == "video") {
                pending = m_videoManager->Tick(GetPosition());
                if (pending && !(pending->valid)) pending = nullptr;
            }

            // ── Hot-reload: detect static image changes ──────────────
            // R1: this MUST run outside BeginFrame/EndFrame — LoadTexture
            // takes m_renderMutex, which BeginFrame holds for the whole
            // frame (same-thread reentry used to self-deadlock). It also
            // runs before the dirty gate so static scenes (which usually
            // early-continue below) actually pick up file changes (R19).
            if (m_state == EngineState::Running
                && !renderHotReloadPath.empty()
                && m_frameCount % 60 == 0) {
                std::error_code ec;
                auto lastWrite = std::filesystem::last_write_time(renderHotReloadPath, ec);
                if (!ec && lastWrite != renderHotReloadLastWrite) {
                    if (m_renderer->LoadTexture(renderHotReloadPath).IsSuccess()) {
                        // Publish the new timestamp under the lock so the
                        // LoadScene seed and this writer never race.
                        std::lock_guard<std::mutex> stampLock(m_mutex);
                        m_hotReloadLastWrite = lastWrite;
                        REX_LOG_INFO("Engine", "Hot-reloaded static image: {}",
                                     renderHotReloadPath);
                        // New pixels: make sure the next pass presents them
                        // even when nothing else armed the dirty flag.
                        m_frameDirty.store(true, std::memory_order_release);
                    }
                }
            }

            // Phase 3: video presents at wall cadence. A new frame draws
            // immediately; with nothing new due the cached frame
            // re-presents (repeat) so cadence holds while unique rate
            // follows decode. Only when nothing ever decoded do we idle.
            const bool isVideoScene = (m_state == EngineState::Running
                && m_videoManager && m_videoManager->IsOpen()
                && renderSceneType == "video");
            if (isVideoScene) {
                if (!pending && !m_hasVideoFrame.load(std::memory_order_acquire)
                    && !m_frameDirty.load(std::memory_order_acquire)) {
                    m_frameCount++;
                    PaceLoop(frameStart, true);
                    continue;
                }
            } else if (!pending && !m_frameDirty.load(std::memory_order_acquire)) {
                // P2-16: spectrum sampling below must keep running for
                // audio-reactive scenes — with no video frame and a clean
                // flag the gate would otherwise freeze the visualization.
                // Non-audio statics still idle (R2).
                const bool spectrumActive = renderAudioCapture &&
                                            renderAudioCapture->IsRunning();
                if (!spectrumActive) {
                    m_frameCount++;          // keep %30/%120 housekeeping alive
                    PaceLoop(frameStart, false);
                    continue;
                }
            }

            // P0-4: a false return means the GPU did not release a frame
            // context in time (device flagged removed) — skip this pass so
            // nothing is recorded into a stale list; recovery runs next.
            if (!m_renderer->BeginFrame()) {
                m_frameCount++;
                PaceLoop(frameStart, isVideoScene);
                continue;
            }

            if (pending && m_renderer) {
                m_renderer->DrawVideoFrame(*pending,
                                           m_videoManager->GetSharedFence());
                if (IsMultiMonitorMode()) {
                    // Same decoded frame to every monitor compositor.
                    m_videoManager->PresentToAllMonitors(*pending);
                }
                // Phase 3: cache for cadence re-presents; count the unique.
                m_cachedVideoFrame = *pending;
                m_hasVideoFrame.store(true, std::memory_order_release);
                m_uniqueVideoFrames.fetch_add(1, std::memory_order_relaxed);
                // New pixels fully handled this pass — disarm any setter-
                // sourced dirty so we don't double-present.
                m_frameDirty.store(false, std::memory_order_release);
            } else if (isVideoScene && m_renderer && m_videoManager
                       && m_hasVideoFrame.load(std::memory_order_acquire)) {
                // Phase 3: nothing new due — re-draw the cached frame at
                // wall cadence. The slot fence is monotonic, so waiting on
                // the stored (older-or-equal) value is already satisfied.
                m_renderer->DrawVideoFrame(m_cachedVideoFrame,
                                           m_videoManager->GetSharedFence());
                if (IsMultiMonitorMode()) {
                    m_videoManager->PresentToAllMonitors(m_cachedVideoFrame);
                }
                m_repeatedVideoFrames.fetch_add(1, std::memory_order_relaxed);
            }

            // ── Clear color (static dark until texture is loaded) ──────
            // Applied once per texture-less state; re-presenting identical
            // dark frames is exactly what P16 exists to prevent.
            if (m_state == EngineState::Running
                && !m_renderer->HasTexture()) {
                if (!m_darkCleared) {
                    m_renderer->SetClearColor(0.02f, 0.02f, 0.02f);
                    m_darkCleared = true;
                }
            } else {
                m_darkCleared = false;
            }

            // Feed audio spectrum to renderer (zeroed if no audio capture).
            // Uses the per-frame snapshot: the command thread can replace
            // the capture device mid-frame.
            if (renderAudioCapture && renderAudioCapture->IsRunning()) {
                std::array<float, 32> spectrum{};
                if (renderAudioCapture->GetSpectrum(spectrum)) {
                    m_renderer->UpdateAudioSpectrum(spectrum);
                }
            }

            m_renderer->RenderFrame();
            m_renderer->EndFrame();

            // R2: whatever just presented is now on screen — disarm the
            // dirty flag for every scene type, not just video. Without
            // this, static scenes re-present at full loop rate forever.
            m_frameDirty.store(false, std::memory_order_release);

            auto frameEnd = high_resolution_clock::now();
            double deltaMs = duration<double, std::milli>(
                frameEnd - frameStart).count();
            Update(deltaMs / 1000.0);

            m_presentCount.fetch_add(1, std::memory_order_relaxed);
            PaceLoop(frameStart, isVideoScene);
        } else {
            // Renderer not ready — wait before retrying
            std::this_thread::sleep_for(milliseconds(16));
        }
    }

    REX_LOG_INFO("Engine", "Render loop ended");
}

// ─── P16 content-aware pacing ──────────────────────────────────────────────
//
// Sleeps until the next expected frame. Target interval is the SLOWER of
// maxFps and the decoder's measured cadence, floored at ~15 Hz so pause/
// resume/unload/SET_* commands stay responsive.

void Engine::PaceLoop(
    std::chrono::high_resolution_clock::time_point frameStart,
    bool isVideoScene) {
    // P2-15: refresh the cached decoder cadence at most twice/sec
    // (telemetry does percentile ring math — not per-wakeup work).
    const auto now = std::chrono::high_resolution_clock::now();
    if (now - m_cadenceSampledAt > std::chrono::milliseconds(500) &&
        m_videoManager && m_videoManager->IsOpen()) {
        m_cadenceSampledAt = now;
        m_cachedDecoderFps = m_videoManager->GetTelemetry().decoderFps;
    }
    double targetIntervalMs;
    if (isVideoScene && m_videoManager && m_videoManager->IsOpen()) {
        // Phase 3: maximum-useful presentation cadence. The decoder still
        // paces itself to the source rate; this only sets how often we
        // present (new frame or cached re-present).
        const int userCap = m_config.maxFps > 0 ? m_config.maxFps : 60;
        targetIntervalMs =
            1000.0 / CadenceController::PresentationTarget(
                         m_videoManager->GetFrameRate(),
                         m_speed.load(std::memory_order_relaxed),
                         m_displayHzCached.load(std::memory_order_acquire),
                         userCap);
    } else {
        targetIntervalMs =
            RenderIntervalMs(m_config.maxFps, m_cachedDecoderFps);
    }
    const auto deadline = frameStart +
        std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
            std::chrono::duration<double, std::milli>(targetIntervalMs));
    rex::shared::PreciseSleepUntil(deadline);
}

// ─── Per-monitor wallpapers (Phase 5) ─────────────────────────────────────

void Engine::SetupMonitorCompositors() {
    if (!m_videoManager || !IsMultiMonitorMode()) return;

    graphics::CommandQueue* queue = m_renderer ? &m_renderer->GetCommandQueue() : nullptr;
    for (const auto& info : EnumerateMonitors()) {
        auto result = m_videoManager->AddMonitor(info, queue);
        if (result.IsError()) {
            REX_LOG_WARN("Engine", "Failed to add compositor for monitor '{}': {}",
                         info.id.c_str(), result.Error().message);
        }
    }
    m_videoManager->RestoreMonitorAssignments();
}

shared::Result<void> Engine::SetPerMonitorWallpaper(const std::string& monitorId,
                                                    const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_videoManager) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }
    return m_videoManager->SetPerMonitorWallpaper(monitorId, path);
}

void Engine::SetSettingsService(
    std::shared_ptr<rex::settings::ISettingsService> settings) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_videoManager) {
        m_videoManager->SetSettingsService(std::move(settings));
    }
}

} // namespace rex::wallpaper
