#include "NotificationService.hpp"
#include "modules/EventBus.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"

namespace rex::notifications {

NotificationService::NotificationService(rex::modules::EventBus& bus,
                                         std::shared_ptr<rex::settings::ISettingsService> settingsService)
    : manager_(bus, *this), settings_(std::move(settingsService))
{
    if (settings_) {
        auto maxVis = settings_->getValue("notifications.max_visible");
        if (maxVis.has_value() && std::holds_alternative<int64_t>(*maxVis)) {
            manager_.setMaxVisible(static_cast<size_t>(std::get<int64_t>(*maxVis)));
        }
        auto dur = settings_->getValue("notifications.default_duration_ms");
        if (dur.has_value() && std::holds_alternative<int64_t>(*dur)) {
            manager_.setDefaultDuration(std::chrono::milliseconds(std::get<int64_t>(*dur)));
        }
    }
}

void NotificationService::setRenderer(std::unique_ptr<INotificationRenderer> renderer) {
    manager_.setRenderer(std::move(renderer));
}

std::string NotificationService::show(const Notification& n) {
    return manager_.showNotification(n);
}

bool NotificationService::update(const std::string& id, const Notification& updated) {
    return manager_.updateNotification(id, updated);
}

void NotificationService::dismiss(const std::string& id) {
    manager_.dismissNotification(id);
}

void NotificationService::dismissAll() {
    manager_.dismissAll();
}

std::vector<Notification> NotificationService::getDisplayed() {
    return manager_.getDisplayedNotifications();
}

std::optional<Notification> NotificationService::getById(const std::string& id) {
    return manager_.getNotification(id);
}

void NotificationService::invokeAction(const std::string& notificationId, const std::string& actionId) {
    manager_.invokeAction(notificationId, actionId);
}

std::vector<Notification> NotificationService::getHistory(const std::string& moduleFilter,
                                                          NotificationType typeFilter) {
    return manager_.getHistory(moduleFilter, typeFilter);
}

void NotificationService::clearHistory() {
    manager_.clearHistory();
}

void NotificationService::update(float deltaTime) {
    manager_.update(deltaTime);
}

}
