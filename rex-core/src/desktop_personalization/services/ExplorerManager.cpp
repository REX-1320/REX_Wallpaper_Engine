#include "ExplorerManager.hpp"

namespace rex::desktop_personalization {

ExplorerManager::ExplorerManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                                 std::shared_ptr<rex::modules::EventBus> bus)
    : platform_(std::move(platform)), bus_(std::move(bus)) {}

bool ExplorerManager::restartExplorer() {
    return platform_->restartExplorer();
}

bool ExplorerManager::refreshShell() {
    return platform_->refreshDesktop();
}

} // namespace rex::desktop_personalization
