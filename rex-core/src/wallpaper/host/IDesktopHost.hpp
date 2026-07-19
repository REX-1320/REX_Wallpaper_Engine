#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <functional>
#include <string_view>

namespace rex::wallpaper::host {

// ─── IDesktopHost ───────────────────────────────────────────────────────────
//
// Abstract interface for desktop hosting strategies.
//
// Each implementation wraps a specific technique for placing rendered
// content behind desktop icons. The wallpaper engine communicates ONLY
// through this interface and never knows which implementation is active.
//
// ## Strategies
//
//   WorkerWHost  —  Classic WorkerW technique (Windows 10).
//                   Creates a render window as a child of the WorkerW
//                   that sits below SHELLDLL_DefView in the desktop
//                   z-order.
//
//   ProgmanHost  —  Progman child-window technique (Windows 11 fallback).
//                   Creates a render window as a direct child of Progman
//                   at HWND_BOTTOM, below the desktop icon layer.
//
// ## Validation
//
// Every implementation must validate its own preconditions in Acquire():
//
//   - Parent window exists and is visible
//   - Parent window size matches the monitor
//   - Render window was created successfully
//   - Render window is positioned at the correct z-order
//   - Render window is visible
//
// If any check fails, Acquire() returns an error with a clear message.
// The caller (DesktopHostFactory) logs the failure and tries the next
// strategy.
//
// ## Thread safety
//
// Acquire() and Release() are not thread-safe. The caller must ensure
// they are called from the same thread (or under a mutex). StartMonitor
// and StopMonitor are thread-safe.

class IDesktopHost {
public:
    virtual ~IDesktopHost() = default;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Acquire the desktop host. Creates the necessary windows and
    /// validates that rendering will be visible behind desktop icons.
    /// Returns an error with a clear, actionable message on failure.
    virtual shared::Result<void> Acquire() = 0;

    /// Release the desktop host. Destroys all owned windows and
    /// resets internal state. Safe to call even if not acquired.
    virtual void Release() = 0;

    // ── Render target ──────────────────────────────────────────────────────

    /// HWND that the D3D12 swap chain should target.
    /// Valid only after Acquire() succeeds. Returns nullptr otherwise.
    virtual HWND GetRenderWindow() const = 0;

    /// Desktop dimensions in pixels. These reflect the primary monitor
    /// and are used by the renderer to size the swap chain buffers.
    virtual int GetWidth()  const = 0;
    virtual int GetHeight() const = 0;

    // ── State ──────────────────────────────────────────────────────────────

    /// True if the host has been acquired and is in a usable state.
    virtual bool IsValid()    const = 0;

    /// True between a successful Acquire() and the next Release().
    virtual bool IsAcquired() const = 0;

    // ── Diagnostics ────────────────────────────────────────────────────────

    /// Short human-readable identifier: "WorkerW", "Progman", etc.
    /// Used in log messages to identify the active strategy.
    virtual std::string_view GetHostType() const = 0;

    // ── Shell / Explorer crash recovery ────────────────────────────────────

    /// Start monitoring the shell process for restarts.
    ///
    /// The callback is invoked from a background thread when the render
    /// window needs to be recreated (e.g., after Explorer.exe crashes
    /// and restarts). Implementations that don't need monitoring (or
    /// where monitoring isn't possible) provide a default no-op.
    ///
    /// The callback MUST NOT block or call back into the host's Acquire()
    /// directly — it should set a flag that the render thread checks.
    virtual void StartMonitor(std::function<void()> /*onRecovery*/) {}

    /// Stop shell monitoring. Blocks until the monitor thread exits.
    virtual void StopMonitor() {}
};

} // namespace rex::wallpaper::host
