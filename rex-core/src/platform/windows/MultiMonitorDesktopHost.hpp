#pragma once

#include "IDesktopHost.hpp"
#include "WorkerWManager.hpp"

#include <memory>

namespace rex::platform::windows {

/// Desktop host for multi-monitor wallpapers. Acquires the same wallpaper
/// WorkerW as WorkerWHost (single render window spanning the virtual
/// desktop), but exposes the WorkerW so the engine can parent one
/// per-monitor child window inside the wallpaper layer.
class MultiMonitorDesktopHost : public IDesktopHost {
public:
    MultiMonitorDesktopHost();
    ~MultiMonitorDesktopHost() override;

    shared::Result<void> Acquire() override;
    void Release() override;

    /// Show/hide the shared render window without releasing the layer
    /// (P0.1). Per-monitor compositors sample the shared decode and present
    /// through the main render window, so hiding it hides all of them.
    void SetVisible(bool visible) override;

    HWND GetRenderWindow() const override;
    int  GetWidth()  const override;
    int  GetHeight() const override;

    bool IsValid()    const override;
    bool IsAcquired() const override;

    std::string_view GetHostType() const override;

    /// The wallpaper WorkerW: parent for per-monitor compositor windows.
    HWND GetWallpaperLayerWindow() const override {
        return m_manager->GetWorkerW();
    }

    void StartMonitor(std::function<void()> onRecovery) override;
    void StopMonitor() override;

    /// The wallpaper WorkerW (same as GetWallpaperLayerWindow).
    HWND GetWorkerW() const { return m_manager->GetWorkerW(); }

private:
    std::unique_ptr<WorkerWManager> m_manager;
    int m_width = 0;
    int m_height = 0;
};

} // namespace rex::platform::windows