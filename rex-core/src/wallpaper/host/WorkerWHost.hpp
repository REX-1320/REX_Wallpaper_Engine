#pragma once

#include "IDesktopHost.hpp"
#include "wallpaper/workerw/WorkerWManager.hpp"
#include <memory>

namespace rex::wallpaper::host {

// ─── WorkerWHost ────────────────────────────────────────────────────────────
//
// Desktop hosting strategy using the classic WorkerW technique.
//
// ## How it works
//
//  1. Finds a blank WorkerW window (no SHELLDLL_DefView child).
//  2. Creates a render window and reparents it into the WorkerW.
//  3. The WorkerW sits beneath SHELLDLL_DefView in the desktop z-order,
//     so content rendered into this window appears behind desktop icons.
//
// ## Validation (performed in Acquire())
//
//  - WorkerWManager::Acquire() succeeds → delegated
//  - WorkerW HWND is non-null → delegated
//  - WorkerW dimensions ≥ 50% of desktop dimensions
//    (catches Windows 11 where WorkerW exists but is a tiny placeholder)
//  - Render window exists, is a valid window, and is visible
//
// ## Recovery
//
//  Explorer.exe crash monitoring is handled by WorkerWManager internally.
//  WorkerWHost delegates StartMonitor/StopMonitor to the manager.

class WorkerWHost : public IDesktopHost {
public:
    WorkerWHost();
    ~WorkerWHost() override;

    // ── IDesktopHost ──────────────────────────────────────────────────────
    shared::Result<void> Acquire() override;
    void Release() override;

    HWND    GetRenderWindow() const override;
    int     GetWidth()  const override;
    int     GetHeight() const override;

    bool    IsValid()    const override;
    bool    IsAcquired() const override;

    std::string_view GetHostType() const override;

    void    StartMonitor(std::function<void()> onRecovery) override;
    void    StopMonitor() override;

private:
    std::unique_ptr<wallpaper::WorkerWManager> m_manager;
    int m_width  = 0;
    int m_height = 0;
};

} // namespace rex::wallpaper::host
