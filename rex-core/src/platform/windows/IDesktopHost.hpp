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
};

} // namespace rex::platform::windows
