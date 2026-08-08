#include "Engine.hpp"
#include "platform/windows/DesktopHostFactory.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <psapi.h>

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
    // Use a temporary unique_ptr so the host is released on any early return.
    auto host = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
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
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to initialize renderer: {}", result.Error().message);
        host->Release();
        return result;
    }

    // Success — transfer ownership to member
    m_desktopHost = std::move(host);

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
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    m_playback.reset();
    m_renderer.reset();
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

    // LoadScene only accepts Initialized/Stopped states; the engine is in
    // Error during recovery, so move to Initialized before reloading.
    SetState(EngineState::Initialized);

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

    m_scenesLoaded.fetch_add(1, std::memory_order_relaxed);
    m_currentScene = scene;
    m_currentPosition = 0.0;

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
    } else if (scene.type == "video" && !scene.sourcePath.empty()) {
        m_playback = std::make_unique<PlaybackController>();
        auto videoResult = m_playback->Open(scene.sourcePath,
                                            m_config.enableHardwareAcceleration);
        if (videoResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to open video '{}': {}",
                         scene.sourcePath.c_str(), videoResult.Error().message);
            m_playback.reset();
        } else {
            // Apply scene-level playback settings immediately.
            m_playback->SetLoop(scene.loop);
            m_playback->SetSpeed(std::clamp(scene.speed, 0.25f, 4.0f));
        }
    }

    SetState(EngineState::Stopped);
    return shared::Result<void>();
}

shared::Result<void> Engine::UnloadScene() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Unloading scene");

    m_scenesReleased.fetch_add(1, std::memory_order_relaxed);

    if (m_playback) {
        m_playback->Close();
        m_playback.reset();
    }

    if (m_renderer) {
        m_renderer->ClearTexture();
    }

    m_adapter.reset();
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
    if (m_playback) {
        m_playback->SetLoop(loop);
    }
    REX_LOG_DEBUG("Engine", "Loop set to {}", loop ? "true" : "false");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speed = std::clamp(speed, 0.25f, 4.0f);
    if (m_playback) {
        m_playback->SetSpeed(m_speed.load());
    }
    REX_LOG_DEBUG("Engine", "Speed set to {:.2f}", m_speed.load());
    return shared::Result<void>();
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
    if (property == "speed") {
        return SetSpeed(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "loop") {
        return SetLoop(value == "true" || value == "1");
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

void Engine::GetTelemetry(EngineTelemetry& out) const {
    out = EngineTelemetry{};

    if (m_playback) {
        PlaybackTelemetry pt = m_playback->GetTelemetry();
        out.framesDecoded  = pt.framesDecoded;
        out.framesConsumed = pt.framesConsumed;
        out.framesDropped  = pt.framesDropped;
        out.decoderFps     = pt.decoderFps;
        out.renderFps      = pt.renderFps;
        out.ringSlotCount  = pt.ringSlotCount;
        out.ringBytes      = pt.ringBytes;
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
    if (m_playback && m_currentScene.type == "video") {
        out.videoWidth  = m_playback->GetWidth();
        out.videoHeight = m_playback->GetHeight();
    }

    out.scenesLoaded   = m_scenesLoaded.load(std::memory_order_relaxed);
    out.scenesReleased = m_scenesReleased.load(std::memory_order_relaxed);

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
        out.privateBytesMB = static_cast<double>(pmc.PrivateUsage)
                           / (1024.0 * 1024.0);
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
    m_state.store(state, std::memory_order_release);
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
            bool wasPlaying = m_playing.load();
            SetState(EngineState::Error);
            m_recoveryCount++;

            auto recoveryResult = ReinitializeRenderer();
            if (recoveryResult.IsSuccess()) {
                if (wasPlaying) Play();
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
            // Tick() returns a pointer into the playback ring buffer
            // (SPSC-guaranteed valid until the producer wraps around);
            // the pixel data is uploaded synchronously before the pointer
            // is dropped, so no per-frame copies are needed.
            if (m_state == EngineState::Running && m_playback
                && m_currentScene.type == "video") {
                const DecodedFrame* frame = m_playback->Tick();
                if (frame && frame->valid && m_renderer) {
                    m_renderer->UpdateTexturePixels(
                        frame->pixels.data(),
                        frame->width,
                        frame->height,
                        frame->rowPitch);
                }
            }

            // ── Clear color (static dark until texture is loaded) ──────
            // Previously animated RGB here produced the "different colors"
            // the user saw instead of the video. Keep it static/neutral.
            if (m_state == EngineState::Running
                && !m_renderer->HasTexture()) {
                m_renderer->SetClearColor(0.02f, 0.02f, 0.02f);
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
