#include "DesktopManager.hpp"

namespace rex::desktop_personalization {

DesktopManager::DesktopManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                               std::shared_ptr<rex::modules::EventBus> bus,
                               rex::modules::ServiceContainer&)
    : platform_(std::move(platform)), bus_(std::move(bus)) {}

bool DesktopManager::applyDesktopSettings(const DesktopSettings& settings) {
    if (!platform_->setDesktopIconSize(settings.iconSize)) return false;
    if (!platform_->setDesktopIconSpacing(settings.iconSpacingHorizontal,
                                          settings.iconSpacingVertical)) return false;
    if (!platform_->setDesktopLabelsVisible(settings.showLabels)) return false;
    if (!platform_->refreshDesktop()) return false;

    bus_->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::DesktopSettingsChanged,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
    return true;
}

DesktopSettings DesktopManager::getCurrentSettings() const {
    return {};
}

} // namespace rex::desktop_personalization
