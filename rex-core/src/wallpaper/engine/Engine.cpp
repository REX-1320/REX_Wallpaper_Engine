#include "Engine.hpp"
#include "wallpaper/engine/RecoveryBackoff.hpp"
#include "wallpaper/engine/DisplayTopology.hpp"
#include "wallpaper/engine/CadencePolicy.hpp"
#include "wallpaper/playback/CadenceController.hpp"
#include "wallpaper/engine/PowerPolicy.hpp"
#include "wallpaper/engine/PlaybackPolicy.hpp"
#include "wallpaper/engine/ResourcePolicy.hpp"
#include "wallpaper/audio/AudioPlayer.hpp"
#include "platform/windows/DesktopHostFactory.hpp"
#include "platform/windows/MonitorEdid.hpp"
#include <shared/PreciseSleep.hpp>
#include <shared/ThreadName.hpp>
#include <iostream>
#include <chrono>
#include <cstdio>
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

    // One process-wide factory (audit 5.1 #11): this helper runs on the
    // telemetry/housekeeping paths, and creating a DXGI factory per call
    // showed up in slow-housekeeping logs. DXGI enumeration is thread-safe;
    // the function-local static initializes exactly once.
    static Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (!factory && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;

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

// GPU video-memory query, one implementation (audit 5.1 #15): the
// media-budget gate and the telemetry snapshot used to carry duplicated
// EnumAdapterByLuid/QueryVideoMemoryInfo chains with two error styles.
struct GpuMemoryQuery {
    bool localOk = false;
    bool nonlocalOk = false;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
    HRESULT hrFactory = E_FAIL;
    HRESULT hrEnum = E_FAIL;
    HRESULT hrAs = E_FAIL;
    HRESULT hrLocal = E_FAIL;
    HRESULT hrNonlocal = E_FAIL;
};

// NOTE: ID3D12Device does NOT implement IDXGIDevice — resolve the adapter
// via its LUID + EnumAdapterByLuid instead.
static GpuMemoryQuery QueryGpuMemory(uint64_t rendererLuid) {
    GpuMemoryQuery q;
    if (rendererLuid == 0) return q;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    q.hrFactory = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(q.hrFactory)) return q;
    LUID luid{};
    luid.LowPart = static_cast<DWORD>(rendererLuid & 0xFFFFFFFFull);
    luid.HighPart = static_cast<LONG>(rendererLuid >> 32);
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    q.hrEnum = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(q.hrEnum)) return q;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    q.hrAs = adapter.As(&adapter3);
    if (FAILED(q.hrAs)) return q;
    q.hrLocal = adapter3->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &q.local);
    q.localOk = SUCCEEDED(q.hrLocal);
    q.hrNonlocal = adapter3->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &q.nonlocal);
    q.nonlocalOk = SUCCEEDED(q.hrNonlocal);
    return q;
}

// P1 resource envelope: LOCAL video-memory budget for the renderer's
// adapter (0 when unqueryable — the gate fails open with a warning).
// Dedicated helper rather than reusing GetTelemetry: that path publishes
// atomics under m_mutex and must not run mid-LoadScene.
static uint64_t QueryLocalVideoMemoryBudget(uint64_t rendererLuid) {
    const GpuMemoryQuery q = QueryGpuMemory(rendererLuid);
    return q.localOk ? static_cast<uint64_t>(q.local.Budget) : 0;
}

Engine::Engine() {
    REX_LOG_INFO("Engine", "Engine instance created");
}

Engine::~Engine() {
    // Audit 2026-10-17, #7: if another thread is mid-Shutdown, wait for it
    // before touching members — the teardown mutates and destroys exactly
    // what this destructor would otherwise race. A state re-check alone
    // would not be enough: the state stays ShuttingDown for the whole
    // teardown window (render-thread join, backend teardown).
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_shutdownCv.wait(lock, [this] {
            return m_state.load(std::memory_order_acquire)
                   != EngineState::ShuttingDown;
        });
    }
    if (m_state.load(std::memory_order_acquire) != EngineState::Uninitialized) {
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

    // Pacing servo rollback switch (Phase B2). Default on; REX_PACING_SERVO=0
    // restores the previous open-loop grid exactly, which is the documented
    // rollback for this phase.
    wchar_t servoEnv[8] = {};
    if (::GetEnvironmentVariableW(L"REX_PACING_SERVO", servoEnv,
                                  static_cast<DWORD>(std::size(servoEnv))) > 0 &&
        servoEnv[0] == L'0') {
        m_pacingServoEnabled = false;
        REX_LOG_INFO("Engine", "Pacing servo DISABLED (REX_PACING_SERVO=0)");
    } else {
        REX_LOG_INFO("Engine", "Pacing servo enabled");
    }
    m_pacingServo.Reset();
    m_pacingPrevValid = false;
    m_pacingServoActive.store(false, std::memory_order_relaxed);

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

    // Start render thread
    m_running = true;
    m_renderThread = std::thread(&Engine::RenderLoop, this);

    // Housekeeping worker: the display-topology probe runs here so it can
    // never block a frame (see HousekeepingLoop).
    m_housekeepingStop.store(false, std::memory_order_release);
    m_housekeepingThread = std::thread(&Engine::HousekeepingLoop, this);

    SetState(EngineState::Initialized);
    m_clock.Start(PositionClock::Now());

    REX_LOG_INFO("Engine", "Initialized successfully (session: {})", m_sessionId.ToString().c_str());
    return shared::Result<void>();
}

shared::Result<void> Engine::Shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);

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

        if (m_state == EngineState::ShuttingDown) {
            // Audit 2026-10-17, #7: a second Shutdown used to fall through
            // and re-run the teardown — a double StopMonitor/Release and a
            // second join of the same std::thread (UB). The first caller
            // owns the teardown; this one waits for it to finish. Reporting
            // success afterwards is truthful: the engine really is shut
            // down. cv.wait releases the mutex, so the owner can always
            // complete (it only re-takes the lock at the very end).
            REX_LOG_INFO("Engine", "Shutdown: waiting for in-progress shutdown");
            m_shutdownCv.wait(lock, [this] {
                return m_state.load(std::memory_order_acquire)
                       != EngineState::ShuttingDown;
            });
            REX_LOG_INFO("Engine", "Shutdown: in-progress shutdown finished");
            return shared::Result<void>();
        }

        REX_LOG_INFO("Engine", "Shutting down...");

        SetState(EngineState::ShuttingDown);
        m_running = false;
        // NOTE: m_mutex is intentionally released here (end of scope) before
        // the teardown below. The render thread (still running until the
        // join below) calls GetPosition() / CheckFullScreenPause() which
        // both re-enter m_mutex — the join must never wait on a lock this
        // thread holds.
    } // <── m_mutex released here

    // Audit 2026-10-17, #14: stop the Explorer monitor OUTSIDE the lock.
    // StopMonitor joins the monitor thread, which may be blocked inside
    // ReacquireWorkerW for seconds (window-message timeouts); holding
    // m_mutex across the join stalled every command/telemetry API for the
    // whole window. Safe without the lock: the monitor thread's only engine
    // touchpoint is the recovery callback — a single atomic store on
    // m_recoveryNeeded (see StartMonitor above) — and the render thread's
    // host accesses go through WorkerWManager's own op lock, which
    // StopMonitor deliberately does not hold while joining.
    m_desktopHost->StopMonitor();

    // The render thread may still be executing its current frame; wait for
    // it to observe m_running==false and exit cleanly.
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    // The housekeeping worker touches the renderer and the video manager, so
    // it must be stopped BEFORE the teardown below. The render thread is
    // already gone, so no new probe can be requested.
    m_housekeepingStop.store(true, std::memory_order_release);
    if (m_housekeepingThread.joinable()) {
        m_housekeepingThread.join();
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
    m_clock.Reset();

    SetState(EngineState::Uninitialized);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Wake concurrent Shutdown callers / the destructor waiting for
        // this teardown to finish (audit 2026-10-17, #7).
        m_shutdownCv.notify_all();
    }
    REX_LOG_INFO("Engine", "Shutdown complete");
    return shared::Result<void>();
}

// ─── Recovery ───────────────────────────────────────────────────────────────

shared::Result<void> Engine::ReinitializeRenderer() {
    REX_LOG_INFO("Engine", "Reinitializing renderer...");

    // Audit 2026-10-17, #14 — this used to hold m_mutex for the entire
    // rebuild: decode-thread joins, GPU drain, host reacquire and device/
    // swapchain creation (seconds on a bad day), stalling every command
    // and telemetry API that needed the lock. The lock now only guards the
    // *swaps*: the dying backends are detached onto locals under it, torn
    // down and rebuilt WITHOUT it, and installed at the end. While they
    // are detached the members are null; command paths null-check, Play/
    // LoadScene stay rejected because the state remains Error for the whole
    // rebuild, and Stop/UnloadScene reject on m_rebuildInProgress (they
    // could otherwise flip the state and let a load run against the
    // not-yet-installed backends).
    // It must NOT be held across the caller either: HandleDeviceLoss
    // reloads the scene afterwards and LoadScene/Play take m_mutex
    // themselves.
    std::unique_ptr<VideoManager> oldVideo;
    std::unique_ptr<D3D12Renderer> oldRenderer;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rebuildInProgress.store(true, std::memory_order_release);

        // Device loss restarts the pacing grid; do not let the servo
        // integrate across the discontinuity.
        m_pacingServo.Reset();
        m_pacingPrevValid = false;
        m_pacingServoActive.store(false, std::memory_order_relaxed);

        // Phase 3: drop the cadence re-present cache before tearing down —
        // slot handles/fence values below belong to the dying backend.
        m_hasVideoFrame.store(false, std::memory_order_release);

        // Swap the backends out; the heavy shutdown of the detached
        // objects runs below without the lock.
        oldVideo = std::move(m_videoManager);
        oldRenderer = std::move(m_renderer);
    }

    // ── Heavy phase: teardown and rebuild with the lock released ───────
    if (oldVideo) {
        oldVideo->ShutdownMonitorCompositors();
        oldVideo->Shutdown();
        oldVideo.reset();
    }
    if (oldRenderer) {
        oldRenderer->Shutdown();
        oldRenderer.reset();
    }

    // Reacquire the desktop host if it was lost (e.g., Explorer restart).
    // If still acquired (device loss only), keep the existing host.
    if (!m_desktopHost || !m_desktopHost->IsAcquired()) {
        auto result = m_desktopHost->Acquire();
        if (result.IsError()) {
            REX_LOG_ERROR("Engine", "Failed to reacquire desktop host: {}",
                          result.Error().message);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rebuildInProgress.store(false, std::memory_order_release);
            return result;
        }
    }

    HWND renderWindow = m_desktopHost->GetRenderWindow();
    int width  = m_desktopHost->GetWidth();
    int height = m_desktopHost->GetHeight();

    auto newRenderer = std::make_unique<D3D12Renderer>();
    // P0-2: same ordering as Initialize — force-SDR must be visible to the
    // swapchain before it is created, not after.
    newRenderer->SetForceSdr(m_config.forceSdr);
    auto result = newRenderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to reinitialize renderer: {}",
                      result.Error().message);
        // Members stay detached — the recovery retry path rebuilds from
        // scratch; until it succeeds, Play/LoadScene reject on Error state.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rebuildInProgress.store(false, std::memory_order_release);
        return result;
    }

    // Recreate the video backend on the new device.
    auto newVideo = std::make_unique<VideoManager>();
    auto vmResult = newVideo->Initialize(m_config, newRenderer->GetDevice().Get());
    if (vmResult.IsError()) {
        REX_LOG_WARN("Engine", "VideoManager re-init failed: {} (video scenes will not play)",
                     vmResult.Error().message);
        newVideo.reset();
    }

    // ── Install the new backends (brief) ───────────────────────────────
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_renderer = std::move(newRenderer);
        m_videoManager = std::move(newVideo);

        // Rebuild per-monitor compositors (device changed) and restore
        // persisted assignments.
        if (m_videoManager) {
            m_videoManager->SetWallpaperHostWindow(
                m_desktopHost->GetWallpaperLayerWindow());
        }
        SetupMonitorCompositors();

        m_rebuildInProgress.store(false, std::memory_order_release);
        // LoadScene only accepts Initialized/Stopped states; the engine is
        // in Error during recovery, so move to Initialized before the
        // caller (HandleDeviceLoss) reloads the scene.
        SetState(EngineState::Initialized);
    }

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
            m_clock.SetBase(savedPosition);
            m_clock.Start(PositionClock::Now());
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

    // Tile has no D3D12 tiling implementation (ComputeFillViewport no-ops
    // it into a silent Stretch): refuse loudly instead of misrendering.
    // All fillMode entries funnel through LoadScene or SetFillMode (which
    // already rejects Tile), so this closes the last silent path. OS
    // wallpapers (Path A) still tile natively via WallpaperStyle.
    if (scene.fillMode == FillMode::Tile) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
            "Tile fill mode is not implemented for engine scenes "
            "(use OS wallpaper Path A for tiling)");
    }

    REX_LOG_INFO("Engine", "Loading scene: {} (type: {}, path: {})",
                 scene.name.c_str(), scene.type.c_str(), scene.sourcePath.c_str());

    m_scenesLoaded.fetch_add(1, std::memory_order_relaxed);
    m_currentScene = scene;
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_clock.SetBase(0.0);
    // Fresh stream, fresh wall clock: the old stopwatch kept running from
    // the PREVIOUS file, so GetPosition() (= base + elapsed) opened hundreds
    // of seconds in and the decoder saw a hugely negative lead → catch-up
    // burst → every apply played fast-forward for seconds ("loads fast",
    // all files). Restart the clock here so wall and content start at zero
    // together.
    m_clock.Start(PositionClock::Now());

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
                // P1 resource envelope: refuse media whose GPU footprint
                // exceeds the renderer's video-memory budget instead of
                // exhausting it mid-playback. Same cleanup + loud failure
                // as a failed open (LOAD -> ok:false, callers skip PLAY).
                const MediaBudget mediaBudget = AssessMediaBudget(
                    m_videoManager->GetWidth(), m_videoManager->GetHeight(),
                    QueryLocalVideoMemoryBudget(
                        m_videoManager->GetD3D12AdapterLuid()));
                if (mediaBudget.unknownBudget) {
                    REX_LOG_WARN("Engine", "Video '{}': {}",
                                 scene.sourcePath.c_str(),
                                 mediaBudget.reason.c_str());
                }
                if (mediaBudget.overBudget) {
                    REX_LOG_WARN("Engine", "Refusing video '{}': {} ({} MB)",
                                 scene.sourcePath.c_str(),
                                 mediaBudget.reason.c_str(),
                                 mediaBudget.poolBytes / (1024 * 1024));
                    m_videoManager->ClearVideoFrames();
                    if (m_renderer) {
                        m_renderer->ClearVideoFrame();
                    }
                    m_currentScene = SceneInfo{};
                    m_playing = false;
                    return shared::Result<void>(
                        shared::ErrorCode::OutOfMemory,
                        "video exceeds GPU video-memory budget: " +
                            mediaBudget.reason);
                }
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
                // Occlusion park starts armed: a LOAD onto a covered desktop
                // parks after the threshold instead of burning full rate.
                m_occlusion.Reset();
                m_occlusion.Arm();
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

    // Drop any GPU frames from a previous video scene. The flag clear plus
    // the epoch bump (audit 2026-10-17, #6) disown the cadence re-present
    // cache and any frame the render thread is still holding from the old
    // scene — the previous video must not flash over the new content
    // before its first frame arrives.
    m_hasVideoFrame.store(false, std::memory_order_release);
    m_videoEpoch.fetch_add(1, std::memory_order_acq_rel);
    if (m_renderer) {
        m_renderer->ClearVideoFrame();
    }

    // P0.1: a loaded scene (static/video/audio) has something to show —
    // make sure the layer is visible (a previous UnloadScene hid it).
    // PLAY arrives as a separate command; visibility is scene-driven.
    if (m_desktopHost) {
        m_desktopHost->SetVisible(true);
    }

    SetState(EngineState::Stopped);
    return shared::Result<void>();
}

shared::Result<void> Engine::UnloadScene() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Audit 2026-10-17, #14: during device-loss recovery the backends are
    // detached and recovery owns the lifecycle until ReinitializeRenderer
    // installs the new ones. An unload mid-rebuild would flip the state to
    // Initialized with the members still null, letting a LoadScene/Play run
    // against the half-built pair — reject like Play does in Error state
    // (#8); the caller can retry once the rebuild finishes (seconds).
    if (m_rebuildInProgress.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
                                    "Engine is recovering (renderer rebuild)");
    }

    REX_LOG_INFO("Engine", "Unloading scene");

    // The pacing grid is about to change discontinuously; carrying the servo
    // across a scene change would feed it one bogus interval sample.
    m_pacingServo.Reset();
    m_pacingPrevValid = false;
    m_pacingServoActive.store(false, std::memory_order_relaxed);

    m_scenesReleased.fetch_add(1, std::memory_order_relaxed);

    if (m_videoManager) {
        m_videoManager->Close();
        m_videoManager->ClearVideoFrames();
    }
    // Phase 3: no scene, no cached re-present.
    m_hasVideoFrame.store(false, std::memory_order_release);
    // Audit 2026-10-17, #6: the scene just changed; any frame the render
    // thread is holding belongs to the old (closed) decoder and must not
    // be drawn past this point.
    m_videoEpoch.fetch_add(1, std::memory_order_acq_rel);

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
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_clock.SetBase(0.0);
    // P0.1: no scene, nothing to show — hide the layer so an OS-drawn
    // static wallpaper (or the plain desktop) shows through instead of a
    // frozen video frame. Stop() intentionally does NOT hide (the frozen
    // snap is the cleared-wallpaper behavior); Shutdown() releases.
    if (m_desktopHost) {
        m_desktopHost->SetVisible(false);
    }
    // A pending recovery request refers to the previous scene: a device
    // re-init racing this unload would otherwise reload the just-
    // discarded video over whatever comes next (image-cover bug).
    m_recoveryNeeded.store(false, std::memory_order_release);
    m_recoveryDelayMs = 0;
    // No scene, no occlusion timing.
    m_occlusion.Reset();

    SetState(EngineState::Initialized);
    return shared::Result<void>();
}

shared::Result<void> Engine::Play() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Audit 2026-10-17, #8 — Play transition table. Legal from:
    //   Initialized (fresh scene load), Stopped (replay / Stop→Play),
    //   Paused (resume), Running (idempotent restart of the clock).
    // Rejected from:
    //   Uninitialized (backend torn down — nothing to play on),
    //   ShuttingDown (teardown owns the backends under the released lock),
    //   Error (device-loss recovery owns the lifecycle; its own re-play
    //   runs through Initialized→Stopped once the new backend is up, so
    //   rejecting Play here never blocks recovery itself).
    if (m_state == EngineState::Uninitialized
        || m_state == EngineState::ShuttingDown
        || m_state == EngineState::Error) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
                                    "Engine is not ready to play");
    }

    if (m_currentScene.sourcePath.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "No scene loaded");
    }

    REX_LOG_INFO("Engine", "Play requested");

    if (m_videoManager && m_currentScene.type == "video") {
        m_videoManager->Play();
    }

    m_playing = true;
    SetState(EngineState::Running);
    m_clock.Start(PositionClock::Now());

    // P16: arm the first present after (re)starting playback.
    m_frameDirty.store(true, std::memory_order_release);
    // User-driven play overrides any auto-pause bookkeeping: a later flag
    // clear must not "resume" (re-pause state is re-evaluated anyway).
    m_autoPaused.store(false, std::memory_order_release);
    // Fresh playback: occlusion timing restarts (a stale park must not
    // survive a user Play; the controller was resumed above).
    m_occlusion.Reset();
    m_occlusion.Arm();
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
    m_clock.Fold(PositionClock::Now(), m_speed.load(std::memory_order_relaxed));
    // User-driven pause: a later auto-resume must not override it.
    m_autoPaused.store(false, std::memory_order_release);
    // A user pause disowns any occlusion park (stale parked flag + timer).
    m_occlusion.Reset();

    return shared::Result<void>();
}

shared::Result<void> Engine::Stop() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Audit 2026-10-17, #8 — Stop is a timeline reset, legal from any live
    // state (Running/Paused/Stopped/Initialized; idempotent) but rejected
    // from Uninitialized (no backend exists), ShuttingDown (teardown
    // owns the backends; a "successful" stop would stamp Stopped onto a
    // dying object), and mid-rebuild (audit 2026-10-17, #14: the backends
    // are detached during ReinitializeRenderer; a Stop that stamped
    // Stopped would make LoadScene/Play legal against the not-yet-
    // installed backends before recovery reloads the scene).
    if (m_state == EngineState::Uninitialized
        || m_state == EngineState::ShuttingDown
        || m_rebuildInProgress.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
                                    "Engine is not running");
    }

    REX_LOG_INFO("Engine", "Stop requested");

    if (m_videoManager) {
        m_videoManager->Stop();
    }

    m_playing = false;
    m_currentPosition.store(0.0, std::memory_order_relaxed);
    m_clock.SetBase(0.0);
    // Phase 3: stopped video must not keep re-presenting its last frame
    // at cadence (a stale wallpaper behind the clear color).
    m_hasVideoFrame.store(false, std::memory_order_release);
    // Audit 2026-10-17, #6: invalidate any frame the render thread is
    // still holding from before this stop.
    m_videoEpoch.fetch_add(1, std::memory_order_acq_rel);
    SetState(EngineState::Stopped);
    m_clock.Reset();
    // User-driven stop: a later auto-resume must not override it.
    m_autoPaused.store(false, std::memory_order_relaxed);
    // Stopped playback has no occlusion timing (parked flag + timer die).
    m_occlusion.Reset();

    return shared::Result<void>();
}

double Engine::GetPosition() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_playing) {
        return m_currentPosition.load(std::memory_order_relaxed);
    }
    double pos = m_clock.Position(PositionClock::Now(),
                                  m_speed.load(std::memory_order_relaxed));
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

            // Use wide-char name for the ID. WideCharToMultiByte returns 0
            // on a too-small buffer (audit 5.1 #10) — the buffer is
            // zero-initialized, so a truncated copy can never read past the
            // null terminator; reject the conversion if it fails.
            char nameBuf[128] = {};
            const int nameLen = WideCharToMultiByte(
                CP_UTF8, 0, mi.szDevice, -1, nameBuf,
                static_cast<int>(sizeof(nameBuf)) - 1, nullptr, nullptr);
            if (nameLen <= 0) {
                return TRUE; // skip this monitor, continue enumeration
            }
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

            // Rendering adapter for the topology predicate (P1): same
            // device name/geometry on a different GPU must reconcile.
            // Best-effort (0 when unresolvable); DXGI factory churn is
            // ~ms per call at housekeeping cadence (precedent: the
            // per-GET_STATUS topology query already pays it).
            info.adapterLuid = QueryDisplayAdapterLuid(info.id);

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

    // Audio layout is a playback property, not a renderer one — the adapter
    // gate only covers draw-side properties.
    const bool isAudioMode = (property == "audioMode");
    if (!supported && !isAudioMode) {
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
    if (property == "audioMode") {
        // Native negotiation already exists in AudioPlayer (ladder fallback +
        // user-notification flag). This exposes it through the protocol so the
        // UI can offer mono/stereo/quad/5.1 instead of leaving the endpoint on
        // whatever Auto happened to pick.
        auto mode = ParseAudioOutputMode(value);
        if (!mode) {
            return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                "Unknown audio mode: " + value +
                " (expected auto|mono|stereo|quad|surround51)");
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_currentScene.audioMode = value;
            // Same rule as SetVolume/SetMute: m_videoManager is swapped by
            // the render thread during recovery — dereference under the
            // lock (audit 2026-10-17, #2).
            if (m_videoManager) {
                m_videoManager->SetAudioOutputMode(*mode);
            }
        }
        REX_LOG_INFO("Engine", "Audio output mode set to {}", value);
        return shared::Result<void>();
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
    // Audit 5.5 #7: the video adapter advertises brightness/contrast/
    // saturation and the runtime forwards SET_BRIGHTNESS/SET_CONTRAST/
    // SET_SATURATION, but without these branches the commands fell through
    // to the final "Unknown property" return even though the adjust
    // pipeline (SetVideoAdjustments) already existed.
    if (property == "brightness") {
        return SetBrightness(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "contrast") {
        return SetContrast(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "saturation") {
        return SetSaturation(static_cast<float>(std::atof(value.c_str())));
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

bool Engine::CaptureFrame(const std::string& pathUtf8) {
    {
        std::lock_guard<std::mutex> lock(m_captureMutex);
        if (m_capturePending) {
            REX_LOG_WARN("Engine", "frame snapshot already in progress");
            return false;
        }
        m_capturePath = pathUtf8;
        m_captureDone = false;
        m_captureOk = false;
        m_capturePending = true;
    }
    // Release: RenderLoop's acquire exchange sees the path written above.
    m_captureRequested.store(true, std::memory_order_release);

    std::unique_lock<std::mutex> lock(m_captureMutex);
    // The render loop services this at the top of its next iteration, so a
    // paced frame (~250 ms worst case) plus one SDR re-render, readback and
    // JPEG encode. The bound only exists so a wedged GPU can never turn a
    // quit into a hang; the app's own SHUTDOWN watchdog is the backstop.
    const bool done = m_captureCv.wait_for(
        lock, std::chrono::seconds(10), [this] { return m_captureDone; });
    if (!done) {
        // Leave m_capturePending set: the request is still queued/running,
        // and a second request must not overwrite its path under the
        // render thread's feet.
        REX_LOG_WARN("Engine", "frame snapshot timed out");
        return false;
    }
    return m_captureOk;
}

void Engine::GetTelemetry(EngineTelemetry& out) const {
    // Locked: the 5 s telemetry sampler runs on its own thread while the
    // render thread may be tearing these objects down in ReinitializeRenderer.
    std::lock_guard<std::mutex> lock(m_mutex);
    GetTelemetryLocked(out);
}

bool Engine::TryGetTelemetry(EngineTelemetry& out) const {
    // Never wait: watchdogs must stay responsive even while a command holds
    // the engine lock. ReinitializeRenderer now releases m_mutex across the
    // heavy teardown/rebuild, so a busy interval is short - but it is not
    // bounded from the watchdog's point of view (audit 5.5 #2).
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    GetTelemetryLocked(out);
    return true;
}

void Engine::GetTelemetryLocked(EngineTelemetry& out) const {
    out = EngineTelemetry{};

    if (m_videoManager) {
        VideoPlaybackTelemetry vt = m_videoManager->GetTelemetry();
        out.framesDecoded  = vt.framesDecoded;
        out.framesConsumed = vt.framesConsumed;
        out.framesDropped  = vt.framesDropped;
        out.decoderFps     = vt.decoderFps;
        out.renderFps      = vt.renderFps;
        out.avSyncDriftMs  = vt.avSyncDriftMs;
        out.avSyncCorrections = vt.avSyncCorrections;
        out.sceneEnded     = vt.sceneEnded;
        out.sourceFps      = vt.sourceFps;
        out.convertFps     = vt.convertFps;
        out.schedulerDrops = vt.schedulerDrops;
        out.schedulerDropsPerSec = vt.schedulerDropsPerSec;
        out.schedulerDropsPeak = vt.schedulerDropsPeak;
        out.loopWraps = vt.loopWraps;
        out.monitorPresents.clear();
        for (const auto& [id, count] : m_videoManager->GetMonitorPresentCounts()) {
            EngineTelemetry::MonitorPresentCount mc;
            mc.monitorId = id;
            mc.presents = count;
            out.monitorPresents.push_back(std::move(mc));
        }
        out.queueDepth     = vt.queueDepth;
        out.occlusionParked = m_occlusion.IsParked();
        out.pacingMs       = vt.pacingMs;
        out.reserveMs          = vt.reserveMs;
        out.decodeAheadRate    = vt.decodeAheadRate;
        out.reserveUnderruns   = vt.reserveUnderruns;
        out.playbackSpeed  = vt.playbackSpeed;
        out.contentRate    = vt.contentRate;
        out.decodePathUnusable = vt.decodePathUnusable;
        out.repeatedFrames =
            m_repeatedVideoFrames.load(std::memory_order_relaxed);
        // Cadence re-presents of the cached frame (Phase 3); increments
        // only when the scheduler had nothing new due.
        out.ringSlotCount  = vt.ringSlotCount;
        out.ringBytes      = vt.ringBytes;
        out.decodeMsP50    = vt.decodeMsP50;
        out.decodeMsP95    = vt.decodeMsP95;
        out.decodeMsP99    = vt.decodeMsP99;
        out.decodeServoCorrMs = vt.decodeServoCorrMs;
        out.vpMsP50        = vt.vpMsP50;
        out.vpMsP95        = vt.vpMsP95;
        out.vpMsP99        = vt.vpMsP99;
        out.sendEagainStalls = vt.sendEagainStalls;
        out.hwDecoderProfiles = vt.hwDecoderProfiles;
        out.hardwareAccelerated = vt.hardwareAccelerated;
    }
    if (m_renderer) {
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
    // NON_LOCAL = shared/system-memory segment) — shared query helper
    // (audit 5.1 #15).
    if (m_renderer && m_videoManager) {
        const uint64_t rendererLuid = m_videoManager->GetD3D12AdapterLuid();
        const GpuMemoryQuery q = QueryGpuMemory(rendererLuid);

        // One-shot diagnostic: a failed step means budget==0, which would
        // silently blind every downstream comparison.
        static std::atomic<bool> s_gpuMemWarned{false};
        if (!s_gpuMemWarned.exchange(true) &&
            (rendererLuid == 0 || FAILED(q.hrFactory) || FAILED(q.hrEnum) ||
             FAILED(q.hrAs) || FAILED(q.hrLocal) || FAILED(q.hrNonlocal))) {
            REX_LOG_WARN("Engine",
                         "GPU memory query incomplete: luid={:016x} "
                         "factory={:08x} enum={:08x} as3={:08x} "
                         "local={:08x} nonlocal={:08x}",
                         static_cast<uint64_t>(rendererLuid),
                         static_cast<uint32_t>(q.hrFactory),
                         static_cast<uint32_t>(q.hrEnum),
                         static_cast<uint32_t>(q.hrAs),
                         static_cast<uint32_t>(q.hrLocal),
                         static_cast<uint32_t>(q.hrNonlocal));
        }
        if (q.localOk) {
            out.gpuDedicatedBytes = q.local.CurrentUsage;
            out.gpuBudgetBytes    = q.local.Budget;
        }
        if (q.nonlocalOk) {
            out.gpuSharedBytes = q.nonlocal.CurrentUsage;
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
    // Inter-present-interval distribution. presentFps is an AVERAGE over the
    // window, so a pipeline alternating 37.5/43.75 ms and one locked at 41.667
    // ms report the SAME presentFps. Stutter is jitter, so it is invisible in
    // any mean — the percentiles and the worst gap are what expose it.
    {
        std::array<float, kPresentIntervalRingSize> sorted{};
        size_t n = 0;
        for (size_t i = 0; i < kPresentIntervalRingSize; ++i) {
            const float v = m_presentIntervalRing[i].load(std::memory_order_relaxed);
            if (v > 0.0f) sorted[n++] = v;
        }
        if (n == 0) {
            out.presentGapP50Ms = out.presentGapP95Ms = out.presentGapP99Ms = 0.0f;
            out.presentGapMaxMs = 0.0f;
            out.presentGapJitterMs = 0.0f;
        } else {
            std::sort(sorted.begin(), sorted.begin() + static_cast<long>(n));
            auto pick = [&](double q) {
                size_t i = static_cast<size_t>(q * static_cast<double>(n - 1));
                return sorted[std::min(i, n - 1)];
            };
            out.presentGapP50Ms = pick(0.50);
            out.presentGapP95Ms = pick(0.95);
            out.presentGapP99Ms = pick(0.99);
            double sum = 0.0, sumSq = 0.0;
            for (size_t i = 0; i < n; ++i) {
                sum += sorted[i];
                sumSq += static_cast<double>(sorted[i]) * sorted[i];
            }
            const double mean = sum / static_cast<double>(n);
            const double var = sumSq / static_cast<double>(n) - mean * mean;
            out.presentGapJitterMs = static_cast<float>(std::sqrt(var > 0.0 ? var : 0.0));
            out.presentGapMaxMs = sorted[n - 1];
        }
    }
    out.phaseBeginMeanMs = m_phaseBeginMs.load(std::memory_order_relaxed);
    out.phaseDrawMeanMs  = m_phaseDrawMs.load(std::memory_order_relaxed);
    out.phaseWorkLastMs  = m_phaseWorkMs.load(std::memory_order_relaxed);
    out.phaseWaitLastMs  = m_phaseWaitMs.load(std::memory_order_relaxed);
    out.phaseFrameIndex  = m_phaseFrameIndex.load(std::memory_order_relaxed);
    if (m_renderer) {
        const auto& cd = m_renderer->DisplayCadence();
        // Only publish a dwell figure when DXGI actually converged; otherwise
        // the field reads 0 and cadenceValid is false, which is the honest
        // "unavailable" state (wall_research.md P11).
        out.dwellVblanks = cd.PercentileDwell(1.0);
        out.dwellP50     = cd.PercentileDwell(0.50);
        out.dwellP95     = cd.PercentileDwell(0.95);
        out.dwellP99     = cd.PercentileDwell(0.99);
        out.cadenceValid = cd.HasCadence();
        // Phase B2 DIAGNOSTIC ONLY - not a control input. syncQpc is the DXGI
        // presentation scheduler's own vblank lattice anchor; EstimatedPeriod
        // is the measured lattice pitch from (dSyncQPCTime /
        // dSyncRefreshCount). DWM is the wrong clock for this independent-flip
        // surface (measured 138.6 Hz on a 160 Hz panel), so this is the only
        // in-process non-DWM reference available. Publishing it is what finally
        // lets startup phase be MEASURED rather than inferred.
        out.syncPeriodMs   = cd.EstimatedPeriodMicros() / 1000.0;
        out.syncRefreshCount = cd.LastSyncRefreshCount();
        out.syncPeriodValid = cd.PeriodSamples() > 0;
        // Reserved: no GPU-pressure policy drives this yet (audit 5.1 #12
        // removed the dead atomic); published as a constant so telemetry
        // consumers keep their field.
        out.pressureScale = 1;
        out.pressureWaitMeanMs = m_phaseWaitMeanMs.load(std::memory_order_relaxed);
    }
    out.pacingServoCorrMs = m_pacingCorrMs.load(std::memory_order_relaxed);
    out.pacingServoErrorMs = m_pacingErrorMs.load(std::memory_order_relaxed);
    out.pacingServoIntegralMs = m_pacingIntegralMs.load(std::memory_order_relaxed);
    out.pacingServoActive = m_pacingServoActive.load(std::memory_order_relaxed);
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
    REX_LOG_INFO("Engine", "  Position:        {:.2f}s",
                 m_currentPosition.load(std::memory_order_relaxed));
    REX_LOG_INFO("Engine", "  Frame:           {} (time: {:.2f}ms)",
                 m_frameCount.load(std::memory_order_relaxed),
                 m_frameTime.load(std::memory_order_relaxed));
    REX_LOG_INFO("Engine", "  Recoveries:      {}",
                 m_recoveryCount.load(std::memory_order_relaxed));
    REX_LOG_INFO("Engine", "  Device resets:   {}",
                 m_deviceResetCount.load(std::memory_order_relaxed));
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
        // A/V sync: silent by nature, so it has to be readable. The drift is
        // the CHANGE in the audio-vs-video offset, not the offset itself.
        REX_LOG_INFO("Engine", "  A/V drift:       {:+.1} ms ({} corrections)",
                     tel.avSyncDriftMs, tel.avSyncCorrections);
    }
}

void Engine::Update(double deltaTime) {
    if (m_state != EngineState::Running) return;

    m_frameCount.fetch_add(1, std::memory_order_relaxed);
    m_frameTime.store(deltaTime, std::memory_order_relaxed);

    if (m_playing) {
        // Read the clock directly — this method runs on the render thread,
        // which must never take m_mutex (Shutdown holds it while joining
        // this thread). PositionClock publishes its state for exactly this
        // lock-free read (audit 2026-10-17, #3); rate 1.0 preserves the
        // historical formula here — GetPosition() applies m_speed.
        m_currentPosition.store(m_clock.Position(PositionClock::Now()),
                                std::memory_order_relaxed);

        if (m_frameCount.load(std::memory_order_relaxed) % 60 == 0) {
            REX_LOG_DEBUG("Engine", "Frame {}: {:.2f} s, {:.2f} ms",
                          m_frameCount.load(std::memory_order_relaxed),
                          m_currentPosition.load(std::memory_order_relaxed),
                          deltaTime);
        }
    }
}

// ─── Housekeeping worker ──────────────────────────────────────────────────
//
// The display-topology probe runs here instead of on the render thread: a
// DXGI output enumeration plus GetDpiForMonitor blocked the render thread
// for 415 ms and 1084 ms on a contended desktop (measured 2026-10-20), which
// the wallpaper showed as a one-second freeze. The render thread only
// raises a request flag; this worker picks it up, does the probe and applies
// the result. Nothing on the render thread ever waits for it.
void Engine::HousekeepingLoop() {
    while (!m_housekeepingStop.load(std::memory_order_acquire)) {
        if (!m_housekeepingRequested.exchange(false, std::memory_order_acq_rel)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // A shutdown may have started between the flag and here; the probe
        // touches the renderer and the video manager, so it must not run
        // past the teardown.
        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }
        const auto start = std::chrono::high_resolution_clock::now();
        CheckDisplayChanges();
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - start).count();
        if (ms >= 50.0) {
            REX_LOG_WARN("Engine",
                         "slow housekeeping: CheckDisplayChanges took "
                         "{:.2f} ms (housekeeping thread)",
                         ms);
        }
    }
}

void Engine::CheckDisplayChanges() {    auto monitors = EnumerateMonitors();
    int count = static_cast<int>(monitors.size());

    // Primary geometry is consumed below (assigned-monitor update) — only
    // the m_lastPrimary* members were dead state (audit 5.1 #12).
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
                        // Coverage alone is not fullscreen: with the taskbar
                        // auto-hidden a MAXIMIZED window is exactly the monitor
                        // rect. Distinguish by window style so ordinary
                        // maximized apps (proven live with a maximized
                        // Windows Terminal, 3840x2160) do not freeze the
                        // wallpaper.
                        const LONG style = GetWindowLongPtr(fg, GWL_STYLE);
                        fullScreen = IsTrueFullscreenWindow(
                            static_cast<long>(style), IsZoomed(fg) != FALSE);
                    }
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

    // Diagnostic thread role (ETW/debugger-visible, behavior-neutral).
    // Present runs on this thread, so it covers the REX-Present role too.
    rex::shared::SetCurrentThreadName(L"REX-Render");
    REX_LOG_INFO("Engine", "Render loop started");
    REX_LOG_INFO("Engine", "Desktop host: type={} renderWindow={} {}×{} valid={}",
                 m_desktopHost ? std::string(m_desktopHost->GetHostType()) : "none",
                 m_desktopHost ? fmt::ptr(m_desktopHost->GetRenderWindow()) : nullptr,
                 m_desktopHost ? m_desktopHost->GetWidth() : 0,
                 m_desktopHost ? m_desktopHost->GetHeight() : 0,
                 m_desktopHost ? (m_desktopHost->IsValid() ? "yes" : "no") : "N/A");

    while (m_running) {
        auto frameStart = high_resolution_clock::now();

        // ── Quit-time still (render thread owns the renderer) ───────────
        // The command thread queues the request in CaptureFrame and waits;
        // servicing it here keeps every renderer call on its owning thread
        // and out of the BeginFrame/EndFrame window.
        if (m_captureRequested.exchange(false, std::memory_order_acq_rel)) {
            bool ok = false;
            if (m_renderer && m_renderer->IsInitialized()) {
                ok = m_renderer->CaptureFrameToJpeg(m_capturePath);
            }
            {
                std::lock_guard<std::mutex> lock(m_captureMutex);
                m_captureOk = ok;
                m_captureDone = true;
                m_capturePending = false;
            }
            m_captureCv.notify_all();
        }

        // ── Auto-pause behind full-screen apps (throttled) ──────────────
        if (m_frameCount.load(std::memory_order_relaxed) % 30 == 0) {
            CheckFullScreenPause();
            CheckSuspendSources();
        }

        // ── Service pause/resume requests (lock-free) ──────────────────
        // Unified arbitration (P1 PlaybackPolicy): fullscreen and suspend
        // sources post independently; the effective pause is their OR and
        // resume fires only for OUR pauses (m_autoPaused) — never a
        // user-initiated pause. Serviced here, at the top of the frame,
        // before any GPU work begins. Bodies unchanged by the extraction.
        if (const int fsReq = m_fullScreenRequest.exchange(0, std::memory_order_acq_rel)) {
            m_fullScreenPaused = (fsReq == 1);
        }
        if (const int suspReq = m_suspendRequest.exchange(0, std::memory_order_acq_rel)) {
            m_suspPaused = (suspReq == 1);
        }
        const PlaybackArbitration arb = EvaluatePlayback(PlaybackInputs{
            m_fullScreenPaused, m_suspPaused,
            m_autoPaused.load(std::memory_order_acquire),
            m_state == EngineState::Running,
            m_state == EngineState::Paused});
        if (arb == PlaybackArbitration::AutoPause) {
                // Pause: stop video + mark engine paused (no m_mutex needed;
                // render thread exclusively owns these during normal operation)
                if (m_videoManager) m_videoManager->Pause();
                m_playing = false;
                // Fold first (same reason as Pause()): Start() re-stamps
                // from zero, so the position must be banked or the next
                // resume rewinds it while the decoder kept its own PTS.
                // Rate-scaled like Pause() (Phase 4).
                m_clock.Fold(PositionClock::Now(),
                             m_speed.load(std::memory_order_relaxed));
                SetState(EngineState::Paused);
                m_autoPaused.store(true, std::memory_order_release);
                // Ownership handoff: auto-pause owns the decoder now; any
                // occlusion park (stale flag + timer) is disowned. Resume
                // re-arms below.
                m_occlusion.Reset();
                REX_LOG_INFO("Engine", "Auto-pause applied (fullscreen={}, suspend={})",
                             m_fullScreenPaused, m_suspPaused);
            } else if (arb == PlaybackArbitration::AutoResume) {
                // Resume: restart video + mark engine running
                if (m_videoManager) m_videoManager->Play();
                m_playing = true;
                m_clock.Start(PositionClock::Now());
                SetState(EngineState::Running);
                m_autoPaused.store(false, std::memory_order_release);
                // Fresh timing from here: the decoder just resumed, so a
                // still-covered desktop re-parks after the threshold.
                m_occlusion.Reset();
                m_occlusion.Arm();
                REX_LOG_INFO("Engine", "Auto-resume applied");
            }


        // ── Periodically check for display topology changes ─────────────
        //
        // The probe (a DXGI output enumeration + GetDpiForMonitor) can
        // block for tens of milliseconds - measured at 415 ms and 1084 ms
        // on a contended desktop - and it used to run ON the render thread
        // inside the frame budget, which showed up as a one-second late
        // frame. It is now requested from a dedicated worker thread; the
        // render thread only sets a flag and never waits for it.
        if (m_frameCount.load(std::memory_order_relaxed) % 120 == 0) {
            m_housekeepingRequested.store(true, std::memory_order_release);
        }

        // ── Desktop host / shell restart + device-loss recovery ────────
        // Both sources funnel into ONE gated attempt. Previously each had
        // its own block that `continue`d without pacing and without
        // re-arming the flag: a host that could not be reacquired (Explorer
        // still restarting) spun this thread at 100% CPU, and because the
        // frame counter stopped advancing, ran the %30/%120 housekeeping on
        // every pass. A second hazard: the host request was consumed with
        // exchange(false) at READ time, so a failed attempt's re-armed
        // request was cleared on the next in-window pass and the retry
        // never ran — host-only recovery bricked until restart. The request
        // is now consumed only when an attempt actually runs
        // (DecideRecovery, RecoveryBackoff.hpp).
        const bool hostRecoveryRequested =
            m_recoveryNeeded.load(std::memory_order_acquire);
        const bool deviceLost = m_renderer && m_renderer->IsDeviceRemoved();
        if (hostRecoveryRequested || deviceLost) {
            const auto now = high_resolution_clock::now();
            const auto decision = DecideRecovery(hostRecoveryRequested, deviceLost,
                                                 now >= m_nextRecoveryAttempt);
            if (decision.consumeRequest) {
                m_recoveryNeeded.store(false, std::memory_order_release);
            }
            if (decision.attempt) {
                REX_LOG_INFO("Engine", "Recovery requested ({}), reinitializing...",
                             deviceLost ? "device removed" : "desktop host");
                SetState(EngineState::Error);
                m_recoveryCount.fetch_add(1, std::memory_order_relaxed);
                if (deviceLost) {
                    m_deviceResetCount.fetch_add(1, std::memory_order_relaxed);
                }

                auto recoveryResult = HandleDeviceLoss();
                if (recoveryResult.IsSuccess()) {
                    m_frameCount.store(0, std::memory_order_relaxed);
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
            m_frameCount.fetch_add(1, std::memory_order_relaxed);
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
            // Audit 2026-10-17, #6: consume the scheduler's frame by VALUE.
            // Tick() returns a pointer into the scheduler's ring — memory-
            // stable across Clear(), but the command thread may Close() the
            // decoder (UnloadScene/Stop/LoadScene) at any point after the
            // call. Holding the pointer across the dirty-gate/BeginFrame
            // window (up to a frame period) risks drawing a frame whose
            // pool handles were already released; the epoch check after
            // BeginFrame drops the copy if a teardown ran in between.
            VideoFrame pendingFrame{};
            bool hasPending = false;
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
            uint64_t renderEpoch = 0;
            {
                std::lock_guard<std::mutex> snapshotLock(m_mutex);
                renderSceneType = m_currentScene.type;
                renderHotReloadPath = m_hotReloadPath;
                renderHotReloadLastWrite = m_hotReloadLastWrite;
                renderAudioCapture = m_audioCapture;
                // Same lock as the scene snapshot: every teardown that
                // bumps the epoch also clears/changes the scene under this
                // mutex, so scene and epoch are always observed as a pair
                // (audit 2026-10-17, #6).
                renderEpoch = m_videoEpoch.load(std::memory_order_relaxed);
            }
            if (m_state == EngineState::Running && m_videoManager
                && m_videoManager->IsOpen()
                && renderSceneType == "video") {
                const VideoFrame* ticked = m_videoManager->Tick(GetPosition());
                if (ticked && ticked->valid) {
                    pendingFrame = *ticked;
                    hasPending = true;
                }
            }

            // ── Hot-reload: detect static image changes ──────────────
            // R1: this MUST run outside BeginFrame/EndFrame — LoadTexture
            // takes m_renderMutex, which BeginFrame holds for the whole
            // frame (same-thread reentry used to self-deadlock). It also
            // runs before the dirty gate so static scenes (which usually
            // early-continue below) actually pick up file changes (R19).
            if (m_state == EngineState::Running
                && !renderHotReloadPath.empty()
                && m_frameCount.load(std::memory_order_relaxed) % 60 == 0) {
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
            // ── Occlusion park (vendor-neutral idle) ────────────────────
            // Trackers reflect the presents submitted on previous passes;
            // dt is the pass spacing (clamped — the first pass and any
            // sleep/wake gap would otherwise fake a huge occlusion run).
            // Park/Resume mirror the AutoPause/AutoResume bookkeeping
            // (controller + m_playing + position fold + stopwatch) but keep
            // EngineState::Running so this branch keeps probing; the parked
            // flag — not the state — is the parking signal. Composes with
            // auto-pause by ownership: auto-pause flips the state (which
            // skips this branch) and Resets the policy on both transitions.
            if (isVideoScene) {
                double occlusionDt = 0.0;
                {
                    using seconds_d = std::chrono::duration<double>;
                    const double gap =
                        seconds_d(frameStart - m_occlusionPrevStart).count();
                    if (gap >= 0.0 && gap <= 5.0) occlusionDt = gap;
                }
                m_occlusionPrevStart = frameStart;
                const std::vector<bool> compOccluded =
                    m_videoManager->GetMonitorOcclusionStates();
                const bool allOccluded = AllPathsOccluded(
                    m_renderer->IsPresentOccluded(), compOccluded);
                switch (m_occlusion.Update(allOccluded, occlusionDt)) {
                case OcclusionAction::Park: {
                    if (m_videoManager) m_videoManager->Pause();
                    m_playing = false;
                    m_clock.Fold(PositionClock::Now(),
                                 m_speed.load(std::memory_order_relaxed));
                    REX_LOG_WARN("Engine",
                                 "Occlusion park: all {} present path(s) "
                                 "DWM-occluded — decode+audio paused, probing at 4 Hz",
                                 1 + compOccluded.size());
                    break;
                }
                case OcclusionAction::Resume: {
                    if (m_videoManager) m_videoManager->Play();
                    m_playing = true;
                    m_clock.Start(PositionClock::Now());
                    REX_LOG_WARN("Engine",
                                 "Occlusion resume: present path visible again — "
                                 "decode+audio resumed from parked position");
                    break;
                }
                default:
                    break;
                }
            }
            if (isVideoScene) {
                if (!hasPending && !m_hasVideoFrame.load(std::memory_order_acquire)
                    && !m_frameDirty.load(std::memory_order_acquire)) {
                    m_frameCount.fetch_add(1, std::memory_order_relaxed);
                    if (m_occlusion.IsParked()) {
                        // Parked with nothing decoded yet (LOAD while
                        // covered): 4 Hz probe, not video cadence.
                        std::this_thread::sleep_for(milliseconds(250));
                    } else {
                        PaceLoop(frameStart, true);
                    }
                    continue;
                }
            } else if (!hasPending && !m_frameDirty.load(std::memory_order_acquire)) {
                // P2-16: spectrum sampling below must keep running for
                // audio-reactive scenes — with no video frame and a clean
                // flag the gate would otherwise freeze the visualization.
                // Non-audio statics still idle (R2).
                const bool spectrumActive = renderAudioCapture &&
                                            renderAudioCapture->IsRunning();
                if (!spectrumActive) {
                    m_frameCount.fetch_add(1, std::memory_order_relaxed);
                    PaceLoop(frameStart, false);
                    continue;
                }
            }

            // P0-4: a false return means the GPU did not release a frame
            // context in time (device flagged removed) — skip this pass so
            // nothing is recorded into a stale list; recovery runs next.
            if (!m_renderer->BeginFrame()) {
                m_frameCount.fetch_add(1, std::memory_order_relaxed);
                PaceLoop(frameStart, isVideoScene);
                continue;
            }
            const auto beginFrameEnd = high_resolution_clock::now();

            // Audit 2026-10-17, #6: a teardown (UnloadScene/Stop/LoadScene)
            // may have run between the snapshot above and here — its frames
            // and pool handles are dead. Drop both the pending frame and the
            // re-present cache for this pass; the next scene repaints.
            const bool framesAlive =
                m_videoEpoch.load(std::memory_order_acquire) == renderEpoch;

            if (hasPending && framesAlive && m_renderer) {
                m_renderer->DrawVideoFrame(pendingFrame,
                                           m_videoManager->GetSharedFence());
                if (IsMultiMonitorMode()) {
                    // Same decoded frame to every monitor compositor.
                    m_videoManager->PresentToAllMonitors(pendingFrame);
                }
                // Phase 3: cache for cadence re-presents; count the unique.
                m_cachedVideoFrame = pendingFrame;
                m_hasVideoFrame.store(true, std::memory_order_release);
                m_uniqueVideoFrames.fetch_add(1, std::memory_order_relaxed);
                // New pixels fully handled this pass — disarm any setter-
                // sourced dirty so we don't double-present.
                m_frameDirty.store(false, std::memory_order_release);
            } else if (framesAlive && isVideoScene && m_renderer && m_videoManager
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
            const auto renderEnd = high_resolution_clock::now();
            m_renderer->EndFrame();

            // Slot lease release (Phase D prerequisite): the command list that
            // referenced the decoded pool slot is now recorded and submitted,
            // so the decode thread may reuse that slot. The renderer's
            // GPU-side fence wait already guarantees it has finished READING
            // before this point; this only transfers ownership back.
            if (hasPending && framesAlive && m_videoManager) {
                m_videoManager->NotifyFrameConsumed(pendingFrame.frameIndex);
            }

            // R2: whatever just presented is now on screen — disarm the
            // dirty flag for every scene type, not just video. Without
            // this, static scenes re-present at full loop rate forever.
            m_frameDirty.store(false, std::memory_order_release);

            auto frameEnd = high_resolution_clock::now();
            // Attribute every over-budget frame to a phase. A late frame is
            // invisible in presentFps, so the breakdown is the only way to
            // tell a slow Present from a slow draw from a long sleep.
            // targetMs/gapMs are hoisted: the per-present stage sample below
            // needs them after their blocks close.
            double targetMs = 0.0;
            {
                const double totalMs =
                    duration<double, std::milli>(frameEnd - frameStart).count();
                const double beginMs =
                    duration<double, std::milli>(beginFrameEnd - frameStart).count();
                const double endMs =
                    duration<double, std::milli>(frameEnd - beginFrameEnd).count();
                // Rolling phase means. A present spike shows up as a rising
                // presentMs mean long before it shows up in any percentile.
                m_phaseBeginMs.store(m_phaseBeginMs.load(std::memory_order_relaxed) * 0.9 + beginMs * 0.1,
                                     std::memory_order_relaxed);
                m_phaseDrawMs.store(m_phaseDrawMs.load(std::memory_order_relaxed) * 0.9 + endMs * 0.1,
                                    std::memory_order_relaxed);
                m_phaseWorkMs.store(totalMs, std::memory_order_relaxed);
                m_phaseFrameIndex.store(m_frameCount.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
                if (m_renderer) {
                    m_phaseWaitMs.store(m_renderer->LastFrameLatencyWaitMs(),
                                        std::memory_order_relaxed);
                    // Rolling pressure signal. The frame-latency waitable is
                    // the direct measure of whether the GPU can take frames:
                    // when it blocks, the GPU is contended and the next
                    // present will be late no matter what we do.
                    m_phaseWaitMeanMs.store(
                        m_phaseWaitMeanMs.load(std::memory_order_relaxed) * 0.85 +
                            m_phaseWaitMs.load(std::memory_order_relaxed) * 0.15,
                        std::memory_order_relaxed);
                }
                // gap(n) == targetInterval + work(n): any frame whose WORK
                // exceeds a small budget shows up as a late gap. The target
                // is computed for every pass (not just late ones) because
                // the per-present stage sample below needs it for the
                // deadline flag.
                targetMs =
                    1000.0 / CadenceController::PresentationTarget(
                                 m_videoManager && m_videoManager->IsOpen()
                                     ? m_videoManager->GetFrameRate() : 0.0,
                                 m_speed.load(std::memory_order_relaxed),
                                 m_displayHzCached.load(std::memory_order_acquire),
                                 m_config.maxFps);
                if (totalMs > 12.0) {
                    REX_LOG_WARN("Engine",
                                 "late frame: total={:.2f}ms target={:.2f}ms "
                                 "begin+wake={:.2f}ms draw+present={:.2f}ms "
                                 "latencyWait={:.2f}ms",
                                 totalMs, targetMs, beginMs, endMs,
                                 m_renderer ? m_renderer->LastFrameLatencyWaitMs() : 0.0);
                }
            }
            double deltaMs = duration<double, std::milli>(
                frameEnd - frameStart).count();
            Update(deltaMs / 1000.0);

            m_presentCount.fetch_add(1, std::memory_order_relaxed);
            double gapMs = 0.0;
            {
                const auto gap = std::chrono::duration<double, std::milli>(
                    frameEnd - m_lastPresentAt).count();
                gapMs = gap;
                if (m_lastPresentAt.time_since_epoch().count() != 0 &&
                    gap > 0.0 && gap < 1000.0) {
                    const size_t idx =
                        m_presentIntervalIndex % kPresentIntervalRingSize;
                    m_presentIntervalRing[idx].store(
                        static_cast<float>(gap), std::memory_order_relaxed);
                    m_presentIntervalIndex = idx + 1;
                }
                m_lastPresentAt = frameEnd;
            }
            // Per-present stage sample (diagnostic ring): begin-wait (GPU
            // pressure), draw-submit CPU, end-frame CPU (incl. the Present()
            // call, which blocks on vsync), submit-side gap vs the content
            // target, DXGI dwell, and whether this pass showed a new video
            // frame or a cached re-present. Dumped to perframe_present.csv
            // when REX_PERFRAME_CSV=1 (5 s throttle).
            {
                using namespace std::chrono;
                PresentStage ps{};
                ps.seq = m_stageSeq++;
                ps.frameCount =
                    static_cast<uint64_t>(m_frameCount.load(std::memory_order_relaxed));
                const double waitMs =
                    m_renderer ? m_renderer->LastFrameLatencyWaitMs() : 0.0;
                ps.beginWaitMs_x1000 =
                    static_cast<int64_t>(waitMs * 1000.0 + 0.5);
                ps.drawMs_x1000 = static_cast<int64_t>(
                    duration<double, std::milli>(renderEnd - beginFrameEnd)
                        .count() *
                    1000.0);
                ps.endMs_x1000 = static_cast<int64_t>(
                    duration<double, std::milli>(frameEnd - renderEnd).count() *
                    1000.0);
                ps.gapMs_x1000 = static_cast<int64_t>(gapMs * 1000.0);
                ps.targetMs_x1000 =
                    static_cast<int64_t>(targetMs * 1000.0 + 0.5);
                ps.dwell = m_renderer
                               ? static_cast<int>(m_renderer->LastDwell() + 0.5)
                               : 0;
                ps.presentedNew = (hasPending && framesAlive) ? 1 : 0;
                ps.deadlineMiss =
                    (gapMs > 0.0 && targetMs > 0.0 && gapMs > 1.5 * targetMs)
                        ? 1
                        : 0;
                m_stageRing.Push(ps);
                const auto nowDump = steady_clock::now();
                if (nowDump - m_lastStageDump > seconds(5)) {
                    m_lastStageDump = nowDump;
                    if (std::getenv("REX_PERFRAME_CSV")) {
                        DumpPresentStages();
                    }
                }
            }
            if (m_occlusion.IsParked()) {
                // Parked probe: the one cached-frame present per pass is
                // the uncover detector — 4 Hz, not wall cadence.
                std::this_thread::sleep_for(milliseconds(250));
            } else {
                PaceLoop(frameStart, isVideoScene);
            }
        } else {
            // Renderer not ready — wait before retrying
            std::this_thread::sleep_for(milliseconds(16));
        }
    }

    REX_LOG_INFO("Engine", "Render loop ended");
}

void Engine::DumpPresentStages() const {
    const std::vector<PresentStage> rows = m_stageRing.Snapshot();
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return;
    std::wstring path(tmp);
    path += L"Paiint\\perframe_present.csv";
    FILE* f = nullptr;
    // Plain narrow mode: "ccs=UTF-8" would switch the stream wide-oriented
    // and every narrow fputs below would fail silently (measured: BOM-only
    // file). Content is pure ASCII; no BOM needed.
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return;
    fputs(PresentStageCsvHeader(), f);
    fputc('\n', f);
    for (const auto& s : rows) {
        const std::string row = PresentStageCsvRow(s);
        fputs(row.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
}

// ─── P16 content-aware pacing ──────────────────────────────────────────────
//
// Sleeps until the next expected frame. Target interval is the SLOWER of
// maxFps and the decoder's measured cadence, floored at 4 ms (250 Hz, see
// CadencePolicy::RenderIntervalMs) so pause/resume/unload/SET_* commands
// stay responsive.

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
        // Presentation cadence = the CONTENT rate, not the display rate.
        //
        // Tested 2026-10-11: presenting per-vblank (mpv/Chromium display-sync
        // model) was measured on this machine and REJECTED. At 160 Hz target
        // the median gap did become a clean 6.25 ms, but 4K x 160 presents/s
        // is beyond the GPU: 346 frames exceeded 12 ms of work in 45 s,
        // gapP99 rose 62 -> 72 ms, gapMax reached 98 ms, and effectiveFps
        // FELL 24.0 -> 22.2. Source-rate pacing keeps gapP50 at 41.88 ms
        // (nominal 41.667) with gapP95 at 42.5 ms, i.e. the median and the
        // 95th percentile are already correct. Only the tail is left, and the
        // tail is a draw/present stall, not a cadence error (wall_research.md
        // §0b P5/P7). Presentation rate is therefore left at the content rate;
        // the remaining work is to remove the stall, not to re-grid.
        const int userCap = m_config.maxFps;
        targetIntervalMs =
            1000.0 / CadenceController::PresentationTarget(
                         m_videoManager->GetFrameRate(),
                         m_speed.load(std::memory_order_relaxed),
                         m_displayHzCached.load(std::memory_order_acquire),
                         userCap);
        // GPU-pressure pacing was implemented and then REVERTED (measured
        // 2026-11). The signal works — pressureWaitMeanMs reached 2.92 ms
        // during real app-launch contention, and stayed 0.00 ms at idle, so it
        // is a genuine contention measure. But applying it as a present-rate
        // divisor made effective fps go UP at the moment pressure tripled, on a
        // 4K60 source that was already dropping frames (schedDrops climbing
        // 83 -> 694). It is therefore not shipped: a mitigation that cannot be
        // shown to help is worse than no mitigation.
        // The measurement is retained in telemetry (pressureWaitMeanMs) because
        // it is what any future contention fix must be evaluated against.
    } else {
        targetIntervalMs =
            RenderIntervalMs(m_config.maxFps, m_cachedDecoderFps);
    }
    // ─── Pacing servo (Phase B2) ───────────────────────────────────────
    //
    // Closes the loop on the measured +0.30-0.33 ms per-frame pacing bias.
    // The input is the ACHIEVED interval minus the nominal one, not the
    // v-blank phase: in `deadline = frameStart + interval + corr` the next
    // frame starts at the deadline, so phase and rate are the same knob and
    // steering phase would just trade the rate away (measured: centring phase
    // at 160 Hz costs -2.44% at 24 fps and -5.88% at 60 fps). See
    // shared/PacingServo.hpp and docs/bugs/BASELINE-2026-10-15-phaseA.md.
    double servoCorrMs = 0.0;
    if (m_pacingServoEnabled) {
        const auto actual = std::chrono::duration<double, std::milli>(
            frameStart - m_pacingPrevFrameStart).count();
        if (m_pacingPrevValid && actual > 0.0) {
            servoCorrMs = m_pacingServo.Step(actual - targetIntervalMs,
                                              targetIntervalMs);
        }
        m_pacingPrevFrameStart = frameStart;
        m_pacingPrevValid = true;
        m_pacingCorrMs.store(servoCorrMs, std::memory_order_relaxed);
        m_pacingErrorMs.store(m_pacingServo.FilteredErrorMs(),
                              std::memory_order_relaxed);
        m_pacingIntegralMs.store(m_pacingServo.IntegralMs(),
                                 std::memory_order_relaxed);
        m_pacingServoActive.store(m_pacingServo.Primed(),
                                  std::memory_order_relaxed);
    }
    // NOTE: `corr` is added to this frame's deadline and is NOT cumulative —
    // frameStart is re-measured every frame, so a persistent error yields a
    // persistent bounded offset instead of a ramp. Clamped to +/-10% of the
    // interval inside PacingServo.
    const auto deadline = frameStart +
        std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
            std::chrono::duration<double, std::milli>(
                targetIntervalMs + servoCorrMs));
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
