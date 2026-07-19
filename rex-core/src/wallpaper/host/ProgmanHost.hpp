#pragma once

#include "IDesktopHost.hpp"

#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>

namespace rex::wallpaper::host {

// ─── ProgmanHost ────────────────────────────────────────────────────────────
//
// Desktop hosting strategy using Progman's child-window technique.
//
// ## When this strategy is used
//
// ProgmanHost is the fallback when WorkerWHost fails. This typically
// happens on Windows 11 builds where WorkerW exists but is a tiny
// placeholder window instead of the full-screen wallpaper layer.
//
// ## How it works
//
//  1. Find the Progman (Program Manager) window — the desktop shell
//     window that owns SHELLDLL_DefView (the desktop icon layer).
//  2. Create a full-screen WS_CHILD render window as a direct child
//     of Progman.
//  3. Position it at HWND_BOTTOM in Progman's child z-order, which
//     places it BELOW SHELLDLL_DefView — behind the desktop icons.
//
//   Desktop window hierarchy (Windows 11):
//
//     Progman
//     ├── SHELLDLL_DefView       ← desktop icons (top layer)
//     │     └── SysListView32
//     ├── WorkerW (optional)     ← tiny placeholder, not the wallpaper
//     └── REX Render Window      ← our child at HWND_BOTTOM
//
// ## Validation (performed in Acquire())
//
//  - Progman window exists
//  - Progman is visible
//  - Progman dimensions cover at least 50 % of the desktop
//  - Render window created and is a valid window
//  - Render window is visible
//  - Render window dimensions match the desktop
//
// ## Recovery
//
//  ProgmanHost monitors the shell (Explorer.exe) by polling for
//  Progman window recreation. When Explorer restarts:
//    1. Old Progman is destroyed (our child goes with it)
//    2. New Progman is created by Explorer
//    3. The monitor thread detects the change
//    4. Calls Release() then Acquire() on the new Progman
//    5. Invokes the recovery callback
//
// ## Reliability notes
//
//  The Progman child technique has been used by desktop enhancements
//  since the Windows 95 era. It works on every Windows version because
//  Progman's child z-order (DefView above, everything else below) is
//  a fundamental invariant of the Windows shell.
//
//  However, it is NOT guaranteed to work in all future Windows builds.
//  If Microsoft changes the desktop composition model (e.g., to
//  DirectComposition-only), this strategy may fail — which is why the
//  factory fallback chain exists.

class ProgmanHost : public IDesktopHost {
public:
    ProgmanHost();
    ~ProgmanHost() override;

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
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);

    void MonitorShell();

    // Window handles
    HWND m_progman      = nullptr;
    HWND m_renderWindow = nullptr;

    // Dimensions
    int m_width  = 0;
    int m_height = 0;

    // State
    bool m_acquired = false;
    HINSTANCE m_hInstance = nullptr;

    // Shell monitor
    std::atomic<bool>              m_monitoring{false};
    std::thread                    m_monitorThread;
    std::function<void()>          m_recoveryCallback;
};

} // namespace rex::wallpaper::host
