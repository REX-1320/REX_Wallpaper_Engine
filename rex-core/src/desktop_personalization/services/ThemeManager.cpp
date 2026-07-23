#include "ThemeManager.hpp"

namespace rex::desktop_personalization {

ThemeManager::ThemeManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                           std::shared_ptr<rex::modules::EventBus> bus)
    : platform_(std::move(platform)), bus_(std::move(bus)) {}

bool ThemeManager::applyTheme(const Theme& theme) {
    if (!platform_->setSystemDarkMode(theme.darkMode)) return false;
    if (!platform_->setAccentColor(theme.accentColor)) return false;

    bus_->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::ThemeChanged,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
    return true;
}

bool ThemeManager::setAccentColor(const AccentColor& color) {
    if (!platform_->setAccentColor(color)) return false;
    return true;
}

Theme ThemeManager::getCurrentTheme() const {
    Theme t;
    t.accentColor = platform_->getCurrentAccentColor().value_or(AccentColor::defaultAccent());
    return t;
}

} // namespace rex::desktop_personalization
