#pragma once

#include "wallpaper/playback/VideoPlaybackController.hpp"
#include "wallpaper/monitor/MonitorCompositor.hpp"
#include "graphics/renderer/CommandQueue.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

// ─── VideoManager ───────────────────────────────────────────────────────
//
// Persistent runtime facade for FFmpeg GPU video playback (Phase 2 backend).
// Owns the same-adapter D3D11 device and the shared D3D11/D3D12 fence; every
// published frame lives in a D3D11 pool slot that the D3D12 renderer opens
// through NT shared handles.
//
// D3D12 renderer contract:
//   1. Open each slot once: d3d12Device->OpenSharedHandle(GetSlotHandle(slot))
//   2. Before sampling slot N: d3d12Queue->Wait(GetFence(), GetSlotFence(N))
//      (monotonic values: always race-free, even across ring wrap-around)
//
// Threading: control methods (Open/Play/Pause/Stop/Seek) are safe from any
// thread; Tick() is the render-thread-only consumer (SPSC).

class VideoManager {
public:
    VideoManager() = default;
    ~VideoManager() { Shutdown(); }

    VideoManager(const VideoManager&) = delete;
    VideoManager& operator=(const VideoManager&) = delete;

    /// d3d12Device is borrowed (must outlive this manager). The D3D11
    /// device is created on the exact same adapter so decode textures and
    /// shared handles interoperate with D3D12.
    shared::Result<void> Initialize(const WallpaperConfig& config,
                                    ID3D12Device* d3d12Device);
    /// Tear down everything (video, D3D11 device, shared fence) and
    /// reinitialize on a (possibly new) D3D12 device. Used by device-loss
    /// recovery: the old D3D11/D3D12 resources are dead after the loss.
    shared::Result<void> Reinitialize(const WallpaperConfig& config,
                                      ID3D12Device* d3d12Device);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    shared::Result<void> OpenVideo(std::string_view path);
    void Close();

    /// Adaptive resolution: picks the video stream that best fits the
    /// target monitor resolution (see FfmpegVideoDecoder::SelectStreamByResolution).
    void SetTargetResolution(int targetWidth, int targetHeight);

    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop);
    void SetSpeed(float speed);
    void SetVolume(float volume);
    void SetMute(bool mute);
    shared::Result<void> Seek(double seconds);

    /// Render-thread consumer: latest GPU frame or nullptr.
    const VideoFrame* Tick(double targetPts);

    bool IsOpen()    const { return m_controller && m_controller->IsOpen(); }
    bool IsPlaying() const { return m_controller && m_controller->IsPlaying(); }

    double GetPosition() const { return m_controller ? m_controller->GetPosition() : 0.0; }
    double GetDuration()  const { return m_controller ? m_controller->GetDuration() : 0.0; }
    double GetFrameRate() const { return m_controller ? m_controller->GetFrameRate() : 0.0; }
    int    GetWidth()     const { return m_controller ? m_controller->GetWidth() : 0; }
    int    GetHeight()    const { return m_controller ? m_controller->GetHeight() : 0; }

    // ─── Shared-surface contract (D3D12 renderer) ──────────────────────
    // The D3D12 command queue must Wait on this fence with the frame's
    // fenceValue before sampling a pool slot.
    ID3D12Fence* GetSharedFence() const { return m_fence12.Get(); }
    HANDLE GetSlotHandle(UINT slot) const {
        return m_controller ? m_controller->GetSlotHandle(slot) : nullptr;
    }
    UINT64 GetSlotFence(UINT slot) const {
        return m_controller ? m_controller->GetSlotFence(slot) : 0;
    }

    // ─── Frame surface (cross-process preview) ─────────────────────────
    /// NT handle of the shared D3D11/D3D12 fence (process-local).
    HANDLE GetFenceHandle() const { return m_fenceHandle; }
    /// Adapter LUID as a single 64-bit value (LowPart | HighPart<<32).
    uint64_t GetAdapterLuid() const;
    /// LUID of the adapter the D3D12 renderer runs on (0 = unknown).
    uint64_t GetD3D12AdapterLuid() const;
    /// True when the D3D11 decoder device and the D3D12 renderer device
    /// live on the same physical adapter (required for shared handles).
    bool IsDecoderRendererSameAdapter() const;
    /// Bumped on every OpenVideo/Close; hosts re-duplicate handles on change.
    uint64_t GetSurfaceGeneration() const { return m_surfaceGen; }
    UINT GetSlotCount() const {
        return m_controller ? m_controller->GetSlotCount() : 0;
    }
    VideoPlaybackController::FrameSnapshot GetLastFrame() const {
        return m_controller ? m_controller->GetLastFrame() : VideoPlaybackController::FrameSnapshot{};
    }

    VideoPlaybackTelemetry GetTelemetry() const {
        return m_controller ? m_controller->GetTelemetry() : VideoPlaybackTelemetry{};
    }

    // ─── Per-monitor compositing (Phase 5) ─────────────────────────────
    /// Wallpaper layer window: parent for per-monitor compositor windows.
    /// Only meaningful for the MultiMonitor desktop host.
    void SetWallpaperHostWindow(HWND workerW) { m_workerW = workerW; }

    /// Settings service for per-monitor wallpaper persistence (optional;
    /// persistence is a no-op until this is set). Setting it triggers a
    /// restore of persisted assignments.
    void SetSettingsService(std::shared_ptr<rex::settings::ISettingsService> settings);

    /// Create a compositor for a monitor. Safe to call after Initialize()
    /// (renders on the render thread; control methods are mutex-guarded).
    /// sharedQueue: when set, compositors share the engine renderer's
    /// command queue (cross-renderer race fix).
    shared::Result<void> AddMonitor(const MonitorInfo& info,
                                    graphics::CommandQueue* sharedQueue = nullptr);
    shared::Result<void> RemoveMonitor(const std::string& monitorId);
    /// Refresh geometry (resolution/orientation changes). Recreates the
    /// compositor when the renderer cannot resize.
    void UpdateMonitor(const MonitorInfo& info);

    /// Render-thread: present the just-ticked frame to every monitor
    /// compositor (each does its own Begin/Draw/Render/End).
    void PresentToAllMonitors(const VideoFrame& frame);

    /// Per-monitor wallpaper override. Empty path clears the override
    /// (the monitor falls back to the main scene, which the engine
    /// re-pushes via its LoadScene fan-out).
    shared::Result<void> SetPerMonitorWallpaper(const std::string& monitorId,
                                                const std::string& path);

    /// Load persisted per-monitor assignments (EDID-keyed) and apply them.
    void RestoreMonitorAssignments();

    /// Snapshot of the monitors currently composited (render-thread data).
    std::vector<MonitorInfo> GetActiveMonitors() const;

    /// Fan-out of the main renderer's fill mode.
    void ApplyFillMode(FillMode mode);

    /// Fan-out of the main renderer's transform (rotation/flip).
    void ApplyTransform(float rotationDegrees, bool flipH, bool flipV);

    /// Fan-out of the main renderer's video adjustments.
    void ApplyVideoAdjustments(float brightness, float contrast,
                               float saturation);

    /// Fan-out of the main scene's static texture to monitors WITHOUT a
    /// per-monitor override (they mirror the main scene).
    void SetSharedStaticWallpaper(const std::string& path);
    /// Drop GPU video/static state on all compositors (scene unload).
    void ClearVideoFrames();

    // ─── Per-monitor independent video (Phase 4, item 23) ───────────────
    /// Open a video for a specific monitor (independent playback).
    shared::Result<void> OpenVideoForMonitor(const std::string& monitorId,
                                             std::string_view path);
    /// Play/pause/stop the per-monitor video on the specified monitor.
    void PlayMonitor(const std::string& monitorId);
    void PauseMonitor(const std::string& monitorId);
    void StopMonitor(const std::string& monitorId);
    /// Set loop/speed for a per-monitor video.
    void SetMonitorLoop(const std::string& monitorId, bool loop);
    void SetMonitorSpeed(const std::string& monitorId, float speed);
    // P1-7: per-monitor audio knobs (previously loop/speed only; layered
    // videos had no volume/mute control at all).
    void SetMonitorVolume(const std::string& monitorId, float volume);
    void SetMonitorMute(const std::string& monitorId, bool mute);
    /// Get position/duration of a per-monitor video.
    double GetMonitorPosition(const std::string& monitorId) const;
    /// Mute state of a per-monitor video (proves the single-audio-owner
    /// policy: first opened plays sound, later ones start muted).
    bool IsMonitorVideoMuted(const std::string& monitorId) const;
    double GetMonitorDuration(const std::string& monitorId) const;

    /// Shutdown all monitor compositors (call on shutdown/device loss).
    void ShutdownMonitorCompositors();

    /// Per-monitor present counts (P2 telemetry): monitor id → compositor
    /// presents since creation (0 when the compositor is dormant/absent).
    /// Lock-guarded snapshot; safe from any thread.
    std::vector<std::pair<std::string, uint64_t>> GetMonitorPresentCounts() const;

    /// Per-monitor occlusion states (occlusion park): one flag per active
    /// monitor entry, true when that compositor's last present was dropped
    /// by DWM as fully occluded. Lock-guarded snapshot; safe from any
    /// thread. Order matches GetMonitorPresentCounts.
    std::vector<bool> GetMonitorOcclusionStates() const;

private:
    ID3D12Device* m_d3d12Device = nullptr;  // borrowed
    WallpaperConfig m_config{};

    Microsoft::WRL::ComPtr<ID3D11Device>        m_device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context11;
    Microsoft::WRL::ComPtr<ID3D12Fence>         m_fence12;
    HANDLE m_fenceHandle = nullptr;

    std::unique_ptr<VideoPlaybackController> m_controller;
    bool m_initialized = false;
    std::atomic<uint64_t> m_surfaceGen{0};

    // ─── Shared wallpaper cache ──────────────────────────────────────────
    // MRU bookkeeping of recently used wallpapers (max kMaxCacheEntries).
    // The current controller is reused when the same file is re-requested.
    struct CacheEntry {
        std::string path;
        std::chrono::steady_clock::time_point lastUsed;
    };
    static constexpr size_t kMaxCacheEntries = 3;
    std::deque<CacheEntry> m_recentVideos;
    std::string m_currentPath;

    // ─── Per-monitor compositing ─────────────────────────────────────────
    // Entries are guarded so IPC threads can Add/Remove monitors while the
    // render thread presents (each entry's compositor is owned here).
    struct MonitorEntry {
        MonitorInfo info;
        std::string assignedWallpaper; // per-monitor override ("" = main scene)
        std::unique_ptr<MonitorCompositor> compositor;
        // Per-monitor independent video playback (nullptr = mirrors main scene)
        std::unique_ptr<VideoPlaybackController> monitorController;
    };
    std::vector<MonitorEntry> m_monitorEntries;
    mutable std::mutex m_monitorMutex;
    HWND m_workerW = nullptr;
    graphics::CommandQueue* m_sharedQueue = nullptr; // cross-renderer queue sharing
    std::shared_ptr<rex::settings::ISettingsService> m_settings;

    static std::string AssignmentKey(const MonitorInfo& info);
};

} // namespace rex::wallpaper
