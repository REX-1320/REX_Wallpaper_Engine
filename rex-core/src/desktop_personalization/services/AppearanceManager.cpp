#include "AppearanceManager.hpp"

namespace rex::desktop_personalization {

AppearanceManager::AppearanceManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                                     std::shared_ptr<rex::modules::EventBus> bus)
    : platform_(std::move(platform)), bus_(std::move(bus)) {}

bool AppearanceManager::setDesktopIconSize(int size) {
    return platform_->setDesktopIconSize(size);
}

bool AppearanceManager::setDesktopIconSpacing(int horiz, int vert) {
    return platform_->setDesktopIconSpacing(horiz, vert);
}

bool AppearanceManager::setDesktopLabelsVisible(bool visible) {
    return platform_->setDesktopLabelsVisible(visible);
}

bool AppearanceManager::refreshDesktop() {
    return platform_->refreshDesktop();
}

} // namespace rex::desktop_personalization
