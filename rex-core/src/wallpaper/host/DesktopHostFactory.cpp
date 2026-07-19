#include "DesktopHostFactory.hpp"
#include "WorkerWHost.hpp"
#include "ProgmanHost.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper::host {

// ─── CreateBestAvailable ────────────────────────────────────────────────────

std::unique_ptr<IDesktopHost> DesktopHostFactory::CreateBestAvailable() {
    REX_LOG_INFO("DesktopHost",
        "Searching for available desktop host strategy...");

    // ── Strategy 1: WorkerW (traditional, works on Windows 10) ──────────
    {
        REX_LOG_INFO("DesktopHost", "Trying WorkerW...");

        auto host = std::make_unique<WorkerWHost>();
        auto result = host->Acquire();

        if (result.IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "✓ WorkerW strategy succeeded ({}×{})",
                host->GetWidth(), host->GetHeight());
            return host;
        }

        REX_LOG_WARN("DesktopHost",
            "✗ WorkerW failed: {}", result.Error().message);
    }

    // ── Strategy 2: Progman (fallback for Windows 11+) ──────────────────
    {
        REX_LOG_INFO("DesktopHost", "Trying Progman...");

        auto host = std::make_unique<ProgmanHost>();
        auto result = host->Acquire();

        if (result.IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "✓ Progman strategy succeeded ({}×{})",
                host->GetWidth(), host->GetHeight());
            return host;
        }

        REX_LOG_WARN("DesktopHost",
            "✗ Progman failed: {}", result.Error().message);
    }

    // ── No strategy succeeded ───────────────────────────────────────────
    REX_LOG_ERROR("DesktopHost",
        "All desktop hosting strategies failed. "
        "Tried: WorkerW, Progman. "
        "This typically means Explorer.exe is not running or the "
        "desktop shell is in an unusual state. "
        "Check that Windows is logged in and the desktop is visible.");

    return nullptr;
}

// ─── Create (specific type) ─────────────────────────────────────────────────

std::unique_ptr<IDesktopHost> DesktopHostFactory::Create(
    const std::string& type)
{
    if (type == "WorkerW") {
        return std::make_unique<WorkerWHost>();
    }
    if (type == "Progman") {
        return std::make_unique<ProgmanHost>();
    }

    REX_LOG_ERROR("DesktopHost",
        "Unknown desktop host type: {}", type.c_str());
    return nullptr;
}

} // namespace rex::wallpaper::host
