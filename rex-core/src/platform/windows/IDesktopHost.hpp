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

    virtual void StartMonitor(std::function<void()> /*onRecovery*/) {}
    virtual void StopMonitor() {}
};

} // namespace rex::platform::windows
