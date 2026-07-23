#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <chrono>
#include <memory>
#include "../models/Notification.hpp"
#include "NotificationQueue.hpp"
#include "../history/NotificationHistory.hpp"
#include "../renderer/INotificationRenderer.hpp"
#include "../animation/NotificationAnimation.hpp"
#include "../sound/NotificationSound.hpp"

namespace rex::modules { class EventBus; }

namespace rex::notifications {

class INotificationService;

class NotificationManager {
public:
    NotificationManager(rex::modules::EventBus& bus, INotificationService& service);

    std::string showNotification(const Notification& base);
    bool updateNotification(const std::string& id, const Notification& updated);
    bool dismissNotification(const std::string& id);
    void dismissAll();
    std::optional<Notification> getNotification(const std::string& id) const;
    std::vector<Notification> getDisplayedNotifications() const;

    void invokeAction(const std::string& notificationId, const std::string& actionId);
    std::vector<Notification> getHistory(const std::string& moduleFilter = "",
                                         NotificationType typeFilter = NotificationType::Custom) const;
    void clearHistory();

    void setRenderer(std::unique_ptr<INotificationRenderer> renderer);

    void setMaxVisible(size_t max);
    size_t maxVisible() const;

    void setDefaultDuration(std::chrono::milliseconds duration);
    std::chrono::milliseconds defaultDuration() const;

    void setDuplicateWindow(std::chrono::milliseconds window);
    std::chrono::milliseconds duplicateWindow() const;

    void update(float deltaTime);

private:
    std::string generateId();
    void promoteFromQueue();
    void publishEvent(const std::string& eventType, const std::string& notifId = "");
    Notification* findDuplicate(const Notification& base);

    rex::modules::EventBus& bus_;
    INotificationService& service_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Notification> activeNotifications_;
    NotificationQueue queue_;
    NotificationHistory history_;
    std::unique_ptr<INotificationRenderer> renderer_;
    NotificationAnimation animation_;
    NotificationSound sound_;
    size_t maxVisible_ = 5;
    std::chrono::milliseconds defaultDuration_{5000};
    std::chrono::milliseconds duplicateWindow_{5000};

    uint64_t idCounter_ = 0;
};

}
