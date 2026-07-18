#include "Engine.hpp"
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

    // Acquire WorkerW
    m_workerW = std::make_unique<WorkerWManager>();
    auto result = m_workerW->Acquire();
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to acquire WorkerW: {}", result.Error().message);
        return result;
    }

    HWND renderWindow = m_workerW->GetRenderWindow();
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    // Create Renderer
    m_renderer = std::make_unique<D3D12Renderer>();
    result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to initialize renderer: {}", result.Error().message);
        return result;
    }

    // Start Explorer monitor for WorkerW recovery
    // The callback runs on a background thread — it only sets a flag.
    // The actual recovery happens on the render thread.
    m_workerW->StartMonitor([this]() {
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

    m_workerW->StopMonitor();
    m_running = false;
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    m_playback.reset();
    m_renderer.reset();
    m_workerW->Release();
    m_workerW.reset();

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

    if (!m_workerW || !m_workerW->IsAcquired()) {
        auto result = m_workerW->Acquire();
        if (result.IsError()) {
            REX_LOG_ERROR("Engine", "Failed to reacquire WorkerW: {}",
                          result.Error().message);
            return result;
        }
    }

    HWND renderWindow = m_workerW->GetRenderWindow();
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

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
    if (m_renderer && m_workerW) {
        m_renderer->Resize(
            m_workerW->GetWorkerW() ? GetSystemMetrics(SM_CXSCREEN) : primaryW,
            m_workerW->GetWorkerW() ? GetSystemMetrics(SM_CYSCREEN) : primaryH);
    }
}

void Engine::SetState(EngineState state) {
    m_state = state;
    REX_LOG_DEBUG("Engine", "State changed to {}", static_cast<int>(state));
}

void Engine::RenderLoop() {
    using namespace std::chrono;

    REX_LOG_INFO("Engine", "Render loop started");

    while (m_running) {
        auto frameStart = high_resolution_clock::now();

        // ── Periodically check for display topology changes ─────────────
        if (m_frameCount % 120 == 0) {
            CheckDisplayChanges();
        }

        // ── WorkerW / Explorer restart recovery ───────────────────────
        // The flag is set from the WorkerW monitor thread; we handle it
        // here on the render thread so there's no mutex contention.
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
