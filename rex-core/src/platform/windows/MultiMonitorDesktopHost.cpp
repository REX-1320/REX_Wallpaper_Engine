#include "MultiMonitorDesktopHost.hpp"

#include <shared/Logger.hpp>

namespace rex::platform::windows {

MultiMonitorDesktopHost::MultiMonitorDesktopHost()
    : m_manager(std::make_unique<WorkerWManager>()) {
}

MultiMonitorDesktopHost::~MultiMonitorDesktopHost() {
    Release();
}

shared::Result<void> MultiMonitorDesktopHost::Acquire() {
    if (m_manager->IsAcquired()) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "MultiMonitorDesktopHost already acquired");
    }

    REX_LOG_INFO("MultiMonitorDesktopHost",
        "Acquiring wallpaper WorkerW for multi-monitor hosting...");

    auto result = m_manager->Acquire();
    if (result.IsError()) {
        REX_LOG_ERROR("MultiMonitorDesktopHost",
            "WorkerWManager::Acquire failed: {}", result.Error().message);
        return result;
    }

    HWND workerW = m_manager->GetWorkerW();
    if (!workerW || !IsWindow(workerW)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW handle is null after successful acquisition");
    }

    HWND renderWindow = m_manager->GetRenderWindow();
    if (!renderWindow || !IsWindow(renderWindow)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window is not valid after WorkerW acquisition");
    }

    m_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    REX_LOG_INFO("MultiMonitorDesktopHost",
        "Acquired: WorkerW {}, render window {}, virtual desktop {}x{}",
        fmt::ptr(workerW), fmt::ptr(renderWindow), m_width, m_height);

    return shared::Result<void>();
}

void MultiMonitorDesktopHost::Release() {
    if (!m_manager->IsAcquired()) return;

    REX_LOG_DEBUG("MultiMonitorDesktopHost", "Releasing desktop host...");
    m_manager->Release();
    m_width = 0;
    m_height = 0;
    REX_LOG_INFO("MultiMonitorDesktopHost", "Desktop host released");
}

HWND MultiMonitorDesktopHost::GetRenderWindow() const {
    return m_manager->GetRenderWindow();
}

int  MultiMonitorDesktopHost::GetWidth()  const { return m_width; }
int  MultiMonitorDesktopHost::GetHeight() const { return m_height; }

bool MultiMonitorDesktopHost::IsValid()    const { return m_manager->IsValid(); }
bool MultiMonitorDesktopHost::IsAcquired() const { return m_manager->IsAcquired(); }

std::string_view MultiMonitorDesktopHost::GetHostType() const { return "MultiMonitor"; }

void MultiMonitorDesktopHost::StartMonitor(std::function<void()> onRecovery) {
    m_manager->StartMonitor(std::move(onRecovery));
}

void MultiMonitorDesktopHost::StopMonitor() {
    m_manager->StopMonitor();
}

} // namespace rex::platform::windows