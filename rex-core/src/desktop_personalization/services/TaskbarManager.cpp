#include "TaskbarManager.hpp"

namespace rex::desktop_personalization {

TaskbarManager::TaskbarManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                               std::shared_ptr<rex::modules::EventBus> bus)
    : platform_(std::move(platform)), bus_(std::move(bus)) {}

bool TaskbarManager::setAutoHide(bool autoHide) {
    return platform_->setTaskbarAutoHide(autoHide);
}

bool TaskbarManager::setAlignment(int alignment) {
    return platform_->setTaskbarAlignment(alignment);
}

bool TaskbarManager::refreshTaskbar() {
    return platform_->applyAllChanges();
}

} // namespace rex::desktop_personalization
