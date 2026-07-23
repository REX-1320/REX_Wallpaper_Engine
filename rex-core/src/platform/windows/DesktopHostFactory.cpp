#include "DesktopHostFactory.hpp"
#include "WorkerWHost.hpp"
#include "ProgmanHost.hpp"
#include <shared/Logger.hpp>

namespace rex::platform::windows {

std::unique_ptr<IDesktopHost> DesktopHostFactory::CreateBestAvailable() {
    REX_LOG_INFO("DesktopHost",
        "Searching for available desktop host strategy...");

    {
        REX_LOG_INFO("DesktopHost", "Trying WorkerW...");

        auto host = std::make_unique<WorkerWHost>();
        auto result = host->Acquire();

        if (result.IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "\xe2\x9c\x93 WorkerW strategy succeeded ({}x{})",
                host->GetWidth(), host->GetHeight());
            return host;
        }

        REX_LOG_WARN("DesktopHost",
            "\xe2\x9c\x97 WorkerW failed: {}", result.Error().message);
    }

    {
        REX_LOG_INFO("DesktopHost", "Trying Progman...");

        auto host = std::make_unique<ProgmanHost>();
        auto result = host->Acquire();

        if (result.IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "\xe2\x9c\x93 Progman strategy succeeded ({}x{})",
                host->GetWidth(), host->GetHeight());
            return host;
        }

        REX_LOG_WARN("DesktopHost",
            "\xe2\x9c\x97 Progman failed: {}", result.Error().message);
    }

    REX_LOG_ERROR("DesktopHost",
        "All desktop hosting strategies failed. "
        "Tried: WorkerW, Progman. "
        "This typically means Explorer.exe is not running or the "
        "desktop shell is in an unusual state. "
        "Check that Windows is logged in and the desktop is visible.");

    return nullptr;
}

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

} // namespace rex::platform::windows
