#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <functional>
#include <string_view>

namespace rex::platform::windows {

class IDesktopHost {
public:
    virtual ~IDesktopHost() = default;

    virtual shared::Result<void> Acquire() = 0;
    virtual void Release() = 0;

    /// Show or hide the render window without releasing the layer (P0.1).
    /// Hiding leaves the OS wallpaper visible underneath; the window keeps
    /// its parent/size so showing is instant. Must be safe to call before
    /// Acquire (no-op). Window-thread affine in all implementations.
    virtual void SetVisible(bool visible) = 0;

    virtual HWND GetRenderWindow() const = 0;
    virtual int GetWidth()  const = 0;
    virtual int GetHeight() const = 0;

    virtual bool IsValid()    const = 0;
    virtual bool IsAcquired() const = 0;

    virtual std::string_view GetHostType() const = 0;

    /// Wallpaper layer window (parent of the render window), when the host
    /// exposes one. nullptr otherwise — per-monitor compositing is only
    /// available through a layer the engine can parent windows into.
    virtual HWND GetWallpaperLayerWindow() const { return nullptr; }

    virtual void StartMonitor(std::function<void()> /*onRecovery*/) {}
    virtual void StopMonitor() {}

    /// Session/display power state (P3 / G1+G2). The WorkerW host registers
    /// session + console-display notifications on its render window; other
    /// hosts report false (no suspension). Polled by the engine's suspend
    /// check — see WorkerWManager::WndProc.
    virtual bool IsSessionLocked() const { return false; }
    virtual bool IsDisplayOff() const { return false; }
};

} // namespace rex::platform::windows
