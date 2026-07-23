#include "WorkerWHost.hpp"
#include <shared/Logger.hpp>

namespace rex::platform::windows {

WorkerWHost::WorkerWHost()
    : m_manager(std::make_unique<WorkerWManager>()) {
}

WorkerWHost::~WorkerWHost() {
    Release();
}

shared::Result<void> WorkerWHost::Acquire() {
    if (m_manager->IsAcquired()) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "WorkerWHost already acquired");
    }

    REX_LOG_INFO("WorkerWHost", "Acquiring WorkerW desktop host...");

    auto result = m_manager->Acquire();
    if (result.IsError()) {
        REX_LOG_ERROR("WorkerWHost",
            "WorkerWManager::Acquire failed: {}", result.Error().message);
        return result;
    }

    HWND workerW = m_manager->GetWorkerW();
    if (!workerW) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW handle is null after successful WorkerWManager::Acquire()");
    }

    if (!IsWindow(workerW)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW handle is not a valid window");
    }

    RECT wwRect;
    if (!GetWindowRect(workerW, &wwRect)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "Failed to get WorkerW window dimensions");
    }

    m_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int wwW = wwRect.right - wwRect.left;
    int wwH = wwRect.bottom - wwRect.top;

    if (wwW < m_width / 2 || wwH < m_height / 2) {
        REX_LOG_WARN("WorkerWHost",
            "WorkerW too small for wallpaper: {}x{} (desktop {}x{})",
            wwW, wwH, m_width, m_height);
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW dimensions too small for wallpaper rendering. "
            "The WorkerW found is not the desktop wallpaper layer.");
    }

    HWND renderWindow = m_manager->GetRenderWindow();
    if (!renderWindow || !IsWindow(renderWindow)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window is not a valid window after WorkerW acquisition");
    }

    if (!IsWindowVisible(renderWindow)) {
        REX_LOG_WARN("WorkerWHost",
            "Render window exists but is not visible: {}",
            fmt::ptr(renderWindow));
    }

    REX_LOG_INFO("WorkerWHost",
        "WorkerW acquired: parent {} ({}x{}), render window {}",
        fmt::ptr(workerW), wwW, wwH, fmt::ptr(renderWindow));

    return shared::Result<void>();
}

void WorkerWHost::Release() {
    if (!m_manager->IsAcquired()) return;

    REX_LOG_DEBUG("WorkerWHost", "Releasing WorkerW desktop host...");
    m_manager->Release();
    REX_LOG_INFO("WorkerWHost", "WorkerW released");
}

HWND WorkerWHost::GetRenderWindow() const {
    return m_manager->GetRenderWindow();
}

int  WorkerWHost::GetWidth()  const { return m_width; }
int  WorkerWHost::GetHeight() const { return m_height; }

bool WorkerWHost::IsValid()    const { return m_manager->IsValid(); }
bool WorkerWHost::IsAcquired() const { return m_manager->IsAcquired(); }

std::string_view WorkerWHost::GetHostType() const { return "WorkerW"; }

void WorkerWHost::StartMonitor(std::function<void()> onRecovery) {
    m_manager->StartMonitor(std::move(onRecovery));
}

void WorkerWHost::StopMonitor() {
    m_manager->StopMonitor();
}

} // namespace rex::platform::windows
