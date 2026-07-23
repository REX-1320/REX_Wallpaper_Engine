#pragma once

#include "IDesktopHost.hpp"
#include "WorkerWManager.hpp"
#include <memory>

namespace rex::platform::windows {

class WorkerWHost : public IDesktopHost {
public:
    WorkerWHost();
    ~WorkerWHost() override;

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
    std::unique_ptr<WorkerWManager> m_manager;
    int m_width  = 0;
    int m_height = 0;
};

} // namespace rex::platform::windows
