#include "wallpaper/video/VideoManager.hpp"

#include <shared/Logger.hpp>

#include <cstdio>
#include <string>
#include <dxgi1_6.h>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "VideoManager";
} // namespace

shared::Result<void> VideoManager::Initialize(const WallpaperConfig& config,
                                              ID3D12Device* d3d12Device) {
    if (m_initialized) {
        return shared::Result<void>();  // already initialized
    }
    if (!d3d12Device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "a D3D12 device is required");
    }

    m_d3d12Device = d3d12Device;
    m_config = config;

    // D3D11 device on the SAME adapter as the D3D12 device: hardware video
    // decode + textures shareable with D3D12. The documented D3D12 way to
    // recover the adapter is the device LUID (IDXGIDevice QI is D3D11-only).
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(d3d12Device->GetAdapterLuid(),
                                          IID_PPV_ARGS(&adapter)))) {
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "cannot query D3D12 adapter");
    }
    const UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   createFlags, &fl, 1, D3D11_SDK_VERSION,
                                   &m_device11, nullptr, &m_context11);
    if (FAILED(hr)) {
        char buf[32] = {};
        std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "D3D11 device creation failed (" + std::string(buf) + ")");
    }

    // Shared fence: D3D12 creates, D3D11 opens (orders blt before draw on
    // the same shared pool texture).
    hr = d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                  IID_PPV_ARGS(&m_fence12));
    if (SUCCEEDED(hr)) {
        hr = d3d12Device->CreateSharedHandle(m_fence12.Get(), nullptr,
                                             GENERIC_ALL, nullptr,
                                             &m_fenceHandle);
    }
    if (FAILED(hr)) {
        Shutdown();
        return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                    "shared fence creation failed");
    }

    m_controller = std::make_unique<VideoPlaybackController>();
    m_initialized = true;
    REX_LOG_INFO(kModule, "initialized (D3D11 on D3D12 adapter, hw accel {})",
                 config.enableHardwareAcceleration ? "on" : "off");
    return shared::Result<void>();
}

shared::Result<void> VideoManager::Reinitialize(const WallpaperConfig& config,
                                                ID3D12Device* d3d12Device) {
    Shutdown();
    return Initialize(config, d3d12Device);
}

void VideoManager::Shutdown() {
    if (m_controller) {
        m_controller->Close();
        m_controller.reset();
    }
    if (m_fenceHandle) {
        CloseHandle(m_fenceHandle);
        m_fenceHandle = nullptr;
    }
    m_fence12.Reset();
    // Keep the device alive through the immediate context's teardown: the
    // context's destructor releases its internal device reference and then
    // continues using the device core. Without this extra ref, the core is
    // freed mid-teardown -> use-after-free inside d3d11.dll (intermittent
    // crash calling a freed block at dev-0x890).
    // Use a local ComPtr so the ref is properly Released at scope exit
    // instead of leaking (old code did AddRef without matching Release).
    Microsoft::WRL::ComPtr<ID3D11Device> keepAlive;
    if (m_device11) {
        keepAlive = m_device11;
    }
    m_device11.Reset();
    m_context11.Reset();
    keepAlive.Reset();
    m_d3d12Device = nullptr;
    m_initialized = false;
}

shared::Result<void> VideoManager::OpenVideo(std::string_view path) {
    if (!m_controller) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }

    // Cache hit: the same file is already open — restart instead of reloading.
    if (m_controller->IsOpen() && m_currentPath == path) {
        m_controller->Seek(0.0);
        m_controller->Play();
        return shared::Result<void>();
    }

    // Trim the MRU bookkeeping to the max cache size.
    while (m_recentVideos.size() >= kMaxCacheEntries) {
        m_recentVideos.pop_front();
    }

    m_controller->Close();
    const bool hw = m_config.enableHardwareAcceleration && m_device11;
    shared::Result<void> result = m_controller->Open(path, hw,
                                                     m_context11.Get(), m_fenceHandle);
    if (result.IsError()) {
        m_currentPath.clear();
        return result;
    }
    m_surfaceGen.fetch_add(1, std::memory_order_relaxed);
    m_currentPath = std::string(path);
    m_recentVideos.push_back({m_currentPath, std::chrono::steady_clock::now()});

    // Auto-select the stream closest to the target monitor's resolution.
    {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        const MonitorInfo* target = nullptr;
        for (const auto& entry : m_monitorEntries) {
            if (!entry.info.connected) continue;
            target = &entry.info; // first connected monitor as fallback
            if (entry.info.isPrimary) break; // prefer the primary
        }
        if (target) {
            m_controller->SelectBestStream(target->width, target->height);
        }
    }
    return shared::Result<void>();
}

void VideoManager::Close() {
    if (m_controller) {
        m_controller->Close();
    }
    m_surfaceGen.fetch_add(1, std::memory_order_relaxed);
    m_currentPath.clear();
}

uint64_t VideoManager::GetAdapterLuid() const {
    if (!m_device11) {
        return 0;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device11.As(&dxgiDevice))) {
        return 0;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        return 0;
    }
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
        return 0;
    }
    const LUID luid = desc.AdapterLuid;
    return static_cast<uint64_t>(luid.LowPart) |
           (static_cast<uint64_t>(luid.HighPart) << 32);
}

uint64_t VideoManager::GetD3D12AdapterLuid() const {
    if (!m_d3d12Device) {
        return 0;
    }
    const LUID luid = m_d3d12Device->GetAdapterLuid();
    return static_cast<uint64_t>(luid.LowPart) |
           (static_cast<uint64_t>(luid.HighPart) << 32);
}

bool VideoManager::IsDecoderRendererSameAdapter() const {
    const uint64_t dec = GetAdapterLuid();
    const uint64_t rend = GetD3D12AdapterLuid();
    return dec != 0 && rend != 0 && dec == rend;
}

void VideoManager::SetTargetResolution(int targetWidth, int targetHeight) {
    if (!m_controller || !m_controller->IsOpen()) return;
    m_controller->SelectBestStream(targetWidth, targetHeight);
}

void VideoManager::Play() {
    if (m_controller) m_controller->Play();
}

void VideoManager::Pause() {
    if (m_controller) m_controller->Pause();
}

void VideoManager::Stop() {
    if (m_controller) m_controller->Stop();
}

void VideoManager::SetLoop(bool loop) {
    if (m_controller) m_controller->SetLoop(loop);
}

void VideoManager::SetSpeed(float speed) {
    if (m_controller) m_controller->SetSpeed(speed);
}

void VideoManager::SetVolume(float volume) {
    if (m_controller) m_controller->SetVolume(volume);
}

void VideoManager::SetMute(bool mute) {
    if (m_controller) m_controller->SetMute(mute);
}

shared::Result<void> VideoManager::Seek(double seconds) {
    if (!m_controller) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }
    return m_controller->Seek(seconds);
}

const VideoFrame* VideoManager::Tick(double targetPts) {
    return m_controller ? m_controller->Tick(targetPts) : nullptr;
}

// ─── Per-monitor compositing ─────────────────────────────────────────────

std::string VideoManager::AssignmentKey(const MonitorInfo& info) {
    // EDID hash is the stable identity across reconnects; fall back to the
    // device name when no EDID is available (e.g. remote sessions).
    if (info.edidHash != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(info.edidHash));
        return std::string("monitor.wallpaper.") + buf;
    }
    return "monitor.wallpaper." + info.id;
}

void VideoManager::SetSettingsService(
    std::shared_ptr<rex::settings::ISettingsService> settings) {
    {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        m_settings = std::move(settings);
    }
    // Assignments may have been persisted by a previous session (or the
    // module host); apply them now that the service is available.
    RestoreMonitorAssignments();
}

shared::Result<void> VideoManager::AddMonitor(const MonitorInfo& info,
                                              graphics::CommandQueue* sharedQueue) {
    if (!m_initialized || !m_d3d12Device) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }

    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (const auto& entry : m_monitorEntries) {
        if (entry.info.id == info.id) {
            return shared::Result<void>(); // already composited
        }
    }

    auto compositor = std::make_unique<MonitorCompositor>(info);
    if (m_workerW && IsWindow(m_workerW)) {
        graphics::CommandQueue* queue = sharedQueue ? sharedQueue : m_sharedQueue;
        if (!compositor->Initialize(m_d3d12Device, m_workerW, queue)) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "monitor compositor init failed for " +
                                        info.id);
        }
    } else {
        REX_LOG_WARN("VideoManager",
                     "No wallpaper WorkerW; monitor '{}' compositor is dormant "
                     "(will activate when the host provides a layer)",
                     info.id.c_str());
    }

    m_monitorEntries.push_back({info, "", std::move(compositor)});
    if (sharedQueue) m_sharedQueue = sharedQueue;
    REX_LOG_INFO("VideoManager", "monitor '{}' {}x{} @ ({},{}) added ({} active)",
                 info.id.c_str(), info.width, info.height, info.x, info.y,
                 m_monitorEntries.size());
    return shared::Result<void>();
}

shared::Result<void> VideoManager::RemoveMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto it = m_monitorEntries.begin(); it != m_monitorEntries.end(); ++it) {
        if (it->info.id == monitorId) {
            if (it->compositor) {
                it->compositor->Shutdown();
            }
            m_monitorEntries.erase(it);
            REX_LOG_INFO("VideoManager", "monitor '{}' removed ({} active)",
                         monitorId.c_str(), m_monitorEntries.size());
            return shared::Result<void>();
        }
    }
    return shared::Result<void>(shared::ErrorCode::NotFound,
                                "monitor not found: " + monitorId);
}

void VideoManager::UpdateMonitor(const MonitorInfo& info) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != info.id) continue;

        entry.info = info;
        if (entry.compositor && entry.compositor->IsActive()) {
            entry.compositor->SetPosition(info.x, info.y);
            entry.compositor->Resize(info.width, info.height);
        }
        REX_LOG_DEBUG("VideoManager", "monitor '{}' updated to {}x{} @ ({},{})",
                      info.id.c_str(), info.width, info.height, info.x, info.y);
        return;
    }
}

void VideoManager::PresentToAllMonitors(const VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto& entry : m_monitorEntries) {
        if (!entry.compositor || !entry.compositor->IsActive()) continue;

        // Per-monitor independent video takes priority over shared frame
        if (entry.monitorController && entry.monitorController->IsOpen()) {
            const VideoFrame* monitorFrame = entry.monitorController->Tick(0.0);
            if (monitorFrame && monitorFrame->valid) {
                entry.compositor->PresentFrame(*monitorFrame, m_fence12.Get());
                continue;
            }
        }

        // Fall back to shared frame
        if (frame.valid) {
            entry.compositor->PresentFrame(frame, m_fence12.Get());
        }
    }
}

shared::Result<void> VideoManager::SetPerMonitorWallpaper(const std::string& monitorId,
                                                          const std::string& path) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;

        entry.assignedWallpaper = path;

        if (!entry.compositor || !entry.compositor->IsActive()) {
            return shared::Result<void>();
        }

        if (path.empty()) {
            // Clear the override: the engine re-pushes the main scene via
            // its LoadScene fan-out on the next scene refresh.
            entry.compositor->ClearStaticTexture();
            entry.compositor->ClearVideoFrame();
        } else {
            entry.compositor->PresentStaticTexture(path);
        }

        // Persist (best-effort; no-op without a settings service). The key
        // is dynamic (EDID hash) and unknown at module registration, so
        // register it on first use — addSetting is idempotent per key.
        if (m_settings) {
            rex::settings::Setting dynamicSetting;
            dynamicSetting.id = AssignmentKey(entry.info);
            dynamicSetting.name = "Monitor wallpaper " + entry.info.id;
            dynamicSetting.description =
                "Wallpaper assigned to monitor " + entry.info.id;
            dynamicSetting.type = rex::settings::SettingType::String;
            dynamicSetting.defaultValue = std::string("");
            m_settings->registerSetting(dynamicSetting);
            m_settings->setValue(AssignmentKey(entry.info),
                                 rex::settings::SettingValue(path));
        }
        REX_LOG_INFO("VideoManager", "monitor '{}' wallpaper: {}",
                     monitorId.c_str(),
                     path.empty() ? std::string("(main scene)")
                                  : std::string("\"") + path + "\"");
        return shared::Result<void>();
    }
    return shared::Result<void>(shared::ErrorCode::NotFound,
                                "monitor not found: " + monitorId);
}

void VideoManager::RestoreMonitorAssignments() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    if (!m_settings) {
        REX_LOG_DEBUG("VideoManager",
                      "No settings service; skipping per-monitor restore");
        return;
    }

    for (auto& entry : m_monitorEntries) {
        auto value = m_settings->getValue(AssignmentKey(entry.info));
        if (!value || !std::holds_alternative<std::string>(*value)) {
            continue;
        }
        const std::string& path = std::get<std::string>(*value);
        if (path.empty()) continue;

        entry.assignedWallpaper = path;
        if (entry.compositor && entry.compositor->IsActive()) {
            entry.compositor->PresentStaticTexture(path);
            REX_LOG_INFO("VideoManager", "restored wallpaper for monitor '{}': {}",
                         entry.info.id.c_str(), path.c_str());
        }
    }
}

std::vector<MonitorInfo> VideoManager::GetActiveMonitors() const {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    std::vector<MonitorInfo> out;
    out.reserve(m_monitorEntries.size());
    for (const auto& entry : m_monitorEntries) {
        out.push_back(entry.info);
    }
    return out;
}

void VideoManager::ApplyFillMode(FillMode mode) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->SetFillMode(mode);
        }
    }
}

void VideoManager::ApplyTransform(float rotationDegrees, bool flipH,
                                  bool flipV) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->SetTransform(rotationDegrees, flipH, flipV);
        }
    }
}

void VideoManager::ApplyVideoAdjustments(float brightness, float contrast,
                                         float saturation) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->SetVideoAdjustments(brightness, contrast,
                                                  saturation);
        }
    }
}

void VideoManager::SetSharedStaticWallpaper(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (!entry.compositor || !entry.compositor->IsActive()) continue;
        if (!entry.assignedWallpaper.empty()) continue; // has an override
        entry.compositor->PresentStaticTexture(path);
    }
}

void VideoManager::ClearVideoFrames() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->ClearStaticTexture();
            entry.compositor->ClearVideoFrame();
        }
        if (entry.monitorController) {
            entry.monitorController->Close();
            entry.monitorController.reset();
        }
    }
}

// ─── Per-monitor independent video (Phase 4, item 23) ──────────────────────

shared::Result<void> VideoManager::OpenVideoForMonitor(const std::string& monitorId,
                                                       std::string_view path) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;

        if (!entry.monitorController) {
            entry.monitorController = std::make_unique<VideoPlaybackController>();
        }

        auto result = entry.monitorController->Open(path,
                                                    m_config.enableHardwareAcceleration,
                                                    m_context11.Get(),
                                                    reinterpret_cast<HANDLE>(m_fenceHandle));
        if (result.IsError()) {
            REX_LOG_ERROR("VideoManager",
                          "Failed to open per-monitor video '{}' for '{}': {}",
                          std::string(path), monitorId,
                          result.Error().message);
            entry.monitorController.reset();
            return result;
        }

        entry.monitorController->Play();

        REX_LOG_INFO("VideoManager",
                     "Opened per-monitor video '{}' for monitor '{}'",
                     std::string(path), monitorId);
        return shared::Result<void>();
    }

    return shared::Result<void>(shared::ErrorCode::NotFound,
                                "Monitor not found: " + monitorId);
}

void VideoManager::PlayMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->Play();
        return;
    }
}

void VideoManager::PauseMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->Pause();
        return;
    }
}

void VideoManager::StopMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->Close();
        return;
    }
}

void VideoManager::SetMonitorLoop(const std::string& monitorId, bool loop) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->SetLoop(loop);
        return;
    }
}

void VideoManager::SetMonitorSpeed(const std::string& monitorId, float speed) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->SetSpeed(speed);
        return;
    }
}

void VideoManager::SetMonitorVolume(const std::string& monitorId, float volume) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->SetVolume(volume);
        return;
    }
}

void VideoManager::SetMonitorMute(const std::string& monitorId, bool mute) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        if (entry.monitorController) entry.monitorController->SetMute(mute);
        return;
    }
}

double VideoManager::GetMonitorPosition(const std::string& monitorId) const {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        return entry.monitorController ? entry.monitorController->GetPosition() : 0.0;
    }
    return 0.0;
}

double VideoManager::GetMonitorDuration(const std::string& monitorId) const {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;
        return entry.monitorController ? entry.monitorController->GetDuration() : 0.0;
    }
    return 0.0;
}

void VideoManager::ShutdownMonitorCompositors() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->Shutdown();
        }
    }
    m_monitorEntries.clear();
}

} // namespace rex::wallpaper
