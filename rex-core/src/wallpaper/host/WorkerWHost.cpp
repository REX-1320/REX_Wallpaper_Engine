#include "WorkerWHost.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper::host {

WorkerWHost::WorkerWHost()
    : m_manager(std::make_unique<wallpaper::WorkerWManager>()) {
}

WorkerWHost::~WorkerWHost() {
    Release();
}

// ── Acquire ─────────────────────────────────────────────────────────────────

shared::Result<void> WorkerWHost::Acquire() {
    if (m_manager->IsAcquired()) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "WorkerWHost already acquired");
    }

    REX_LOG_INFO("WorkerWHost", "Acquiring WorkerW desktop host...");

    // Delegate to WorkerWManager for the low-level WorkerW discovery,
    // render window creation, and reparenting.
    auto result = m_manager->Acquire();
    if (result.IsError()) {
        REX_LOG_ERROR("WorkerWHost",
            "WorkerWManager::Acquire failed: {}", result.Error().message);
        return result;
    }

    // ── Validation ─────────────────────────────────────────────────────────
    //
    // Step 1: Validate the WorkerW window itself.
    // On Windows 11, FindBlankWorkerW may return a WorkerW that is
    // only a tiny placeholder (~132×37 px) rather than the actual
    // full-screen wallpaper layer. We reject undersized WorkerWs so
    // the factory can fall through to the next strategy.

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

    m_width  = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);

    int wwW = wwRect.right - wwRect.left;
    int wwH = wwRect.bottom - wwRect.top;

    // Threshold: WorkerW must cover at least 50 % of the desktop in
    // each dimension. If it's smaller, it's almost certainly not the
    // wallpaper layer — it's a placeholder or leftover WorkerW.
    if (wwW < m_width / 2 || wwH < m_height / 2) {
        REX_LOG_WARN("WorkerWHost",
            "WorkerW too small for wallpaper: {}×{} (desktop {}×{})",
            wwW, wwH, m_width, m_height);
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW dimensions too small for wallpaper rendering. "
            "The WorkerW found is not the desktop wallpaper layer.");
    }

    // Step 2: Validate the render window.
    HWND renderWindow = m_manager->GetRenderWindow();
    if (!renderWindow || !IsWindow(renderWindow)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window is not a valid window after WorkerW acquisition");
    }

    if (!IsWindowVisible(renderWindow)) {
        // The render window may legitimately be hidden (WorkerW is a
        // special system window), so this is a warning, not an error.
        REX_LOG_WARN("WorkerWHost",
            "Render window exists but is not visible: {}",
            fmt::ptr(renderWindow));
    }

    REX_LOG_INFO("WorkerWHost",
        "WorkerW acquired: parent {} ({}×{}), render window {}",
        fmt::ptr(workerW), wwW, wwH, fmt::ptr(renderWindow));

    return shared::Result<void>();
}

// ── Release ─────────────────────────────────────────────────────────────────

void WorkerWHost::Release() {
    if (!m_manager->IsAcquired()) return;

    REX_LOG_DEBUG("WorkerWHost", "Releasing WorkerW desktop host...");
    m_manager->Release();
    REX_LOG_INFO("WorkerWHost", "WorkerW released");
}

// ── Accessors ───────────────────────────────────────────────────────────────

HWND WorkerWHost::GetRenderWindow() const {
    return m_manager->GetRenderWindow();
}

int  WorkerWHost::GetWidth()  const { return m_width; }
int  WorkerWHost::GetHeight() const { return m_height; }

bool WorkerWHost::IsValid()    const { return m_manager->IsValid(); }
bool WorkerWHost::IsAcquired() const { return m_manager->IsAcquired(); }

std::string_view WorkerWHost::GetHostType() const { return "WorkerW"; }

// ── Monitoring ──────────────────────────────────────────────────────────────

void WorkerWHost::StartMonitor(std::function<void()> onRecovery) {
    m_manager->StartMonitor(std::move(onRecovery));
}

void WorkerWHost::StopMonitor() {
    m_manager->StopMonitor();
}

} // namespace rex::wallpaper::host
