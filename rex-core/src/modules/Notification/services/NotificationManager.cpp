#include "NotificationManager.hpp"
#include "../interfaces/INotificationService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include <algorithm>

namespace rex::notifications {

NotificationManager::NotificationManager(rex::modules::EventBus& bus, INotificationService& service)
    : bus_(bus), service_(service) {}

void NotificationManager::setRenderer(std::unique_ptr<INotificationRenderer> renderer) {
    std::lock_guard lock(mutex_);
    renderer_ = std::move(renderer);
}

std::string NotificationManager::showNotification(const Notification& base) {
    Notification n = base;
    n.creationTime = std::chrono::steady_clock::now();
    n.state = NotificationState::Pending;

    {
        std::lock_guard lock(mutex_);

        Notification* existing = findDuplicate(n);
        if (existing && existing->state == NotificationState::Displaying) {
            existing->duplicateCount++;
            existing->isGrouped = true;
            publishEvent("updated", existing->id);
            if (renderer_) {
                renderer_->update(*existing);
            }
            return existing->id;
        }

        n.id = generateId();
        queue_.enqueue(n);
        publishEvent("created", n.id);
        promoteFromQueue();
    }

    return n.id;
}

bool NotificationManager::updateNotification(const std::string& id, const Notification& updated) {
    std::lock_guard lock(mutex_);
    auto it = activeNotifications_.find(id);
    if (it != activeNotifications_.end()) {
        it->second = updated;
        it->second.id = id;
        publishEvent("updated", id);
        if (renderer_) {
            renderer_->update(it->second);
        }
        return true;
    }
    return false;
}

bool NotificationManager::dismissNotification(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = activeNotifications_.find(id);
    if (it == activeNotifications_.end()) {
        if (queue_.remove(id)) {
            publishEvent("dismissed", id);
            return true;
        }
        return false;
    }
    it->second.state = NotificationState::Dismissed;

    if (renderer_) {
        Notification dismissed = it->second;
        activeNotifications_.erase(it);
        renderer_->dismiss(id, [this, dismissed]() {
            std::lock_guard lock(mutex_);
            history_.add(dismissed);
            publishEvent("dismissed", dismissed.id);
            promoteFromQueue();
        });
    } else {
        history_.add(it->second);
        publishEvent("dismissed", id);
        activeNotifications_.erase(it);
        promoteFromQueue();
    }
    return true;
}

void NotificationManager::dismissAll() {
    std::lock_guard lock(mutex_);
    for (auto& [id, n] : activeNotifications_) {
        n.state = NotificationState::Dismissed;
        history_.add(n);
        publishEvent("dismissed", id);
    }
    activeNotifications_.clear();
    queue_.clear();
    if (renderer_) {
        renderer_->clearAll();
    }
}

void NotificationManager::invokeAction(const std::string& notificationId, const std::string& actionId) {
    std::lock_guard lock(mutex_);
    auto it = activeNotifications_.find(notificationId);
    if (it == activeNotifications_.end()) return;

    auto& n = it->second;
    for (auto& action : n.actions) {
        if (action.id == actionId && action.enabled) {
            if (action.callback) {
                action.callback(notificationId);
            }
            publishEvent("action_invoked", notificationId);
            break;
        }
    }
}

std::vector<Notification> NotificationManager::getHistory(const std::string& moduleFilter,
                                                          NotificationType typeFilter) const {
    return history_.getAll(moduleFilter, typeFilter);
}

void NotificationManager::clearHistory() {
    history_.clear();
    publishEvent("history_cleared");
}

std::optional<Notification> NotificationManager::getNotification(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = activeNotifications_.find(id);
    if (it != activeNotifications_.end()) return it->second;
    return std::nullopt;
}

std::vector<Notification> NotificationManager::getDisplayedNotifications() const {
    std::lock_guard lock(mutex_);
    std::vector<Notification> result;
    result.reserve(activeNotifications_.size());
    for (const auto& [id, n] : activeNotifications_) {
        result.push_back(n);
    }
    return result;
}

void NotificationManager::setMaxVisible(size_t max) {
    std::lock_guard lock(mutex_);
    maxVisible_ = max;
}

size_t NotificationManager::maxVisible() const {
    std::lock_guard lock(mutex_);
    return maxVisible_;
}

void NotificationManager::setDefaultDuration(std::chrono::milliseconds duration) {
    std::lock_guard lock(mutex_);
    defaultDuration_ = duration;
}

std::chrono::milliseconds NotificationManager::defaultDuration() const {
    std::lock_guard lock(mutex_);
    return defaultDuration_;
}

void NotificationManager::setDuplicateWindow(std::chrono::milliseconds window) {
    std::lock_guard lock(mutex_);
    duplicateWindow_ = window;
}

std::chrono::milliseconds NotificationManager::duplicateWindow() const {
    std::lock_guard lock(mutex_);
    return duplicateWindow_;
}

void NotificationManager::update(float deltaTime) {
    std::vector<std::string> toDismiss;
    {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (const auto& [id, n] : activeNotifications_) {
            if (n.duration.count() == 0 || n.persistent) continue;
            if (now - n.creationTime >= n.duration) {
                toDismiss.push_back(id);
            }
        }
    }
    for (const auto& id : toDismiss) {
        dismissNotification(id);
    }

    if (renderer_) {
        renderer_->tick(deltaTime);
    }
    animation_.tick(deltaTime * 1000.0f);
}

void NotificationManager::promoteFromQueue() {
    while (activeNotifications_.size() < maxVisible_) {
        auto next = queue_.dequeue();
        if (!next) break;
        next->state = NotificationState::Displaying;
        activeNotifications_[next->id] = *next;
        publishEvent("shown", next->id);

        if (renderer_) {
            renderer_->show(*next);
            animation_.start(next->id, AnimationType::Fade, 300.0f);
            sound_.playForNotification(*next);
        }
    }
}

Notification* NotificationManager::findDuplicate(const Notification& base) {
    if (base.groupKey.empty() && base.title.empty() && base.message.empty()) return nullptr;
    auto now = std::chrono::steady_clock::now();
    for (auto& [id, n] : activeNotifications_) {
        if (n.state != NotificationState::Displaying) continue;
        bool sameModule = (n.moduleId == base.moduleId);
        bool sameTitle = (n.title == base.title);
        bool sameMessage = (n.message == base.message);
        if (!base.groupKey.empty()) {
            if (n.groupKey == base.groupKey && sameModule) {
                if (now - n.creationTime <= duplicateWindow_) {
                    return &n;
                }
            }
        } else if (sameModule && sameTitle && sameMessage) {
            if (now - n.creationTime <= duplicateWindow_) {
                return &n;
            }
        }
    }
    return nullptr;
}

std::string NotificationManager::generateId() {
    return "notif_" + std::to_string(++idCounter_);
}

void NotificationManager::publishEvent(const std::string& eventType, const std::string& notifId) {
    rex::modules::ModuleEvent ev;
    ev.moduleId = "rex.notification";
    ev.error = notifId;

    if (eventType == "created") {
        ev.type = rex::modules::ModuleEventType::NotificationCreated;
    } else if (eventType == "shown") {
        ev.type = rex::modules::ModuleEventType::NotificationShown;
    } else if (eventType == "updated") {
        ev.type = rex::modules::ModuleEventType::NotificationUpdated;
    } else if (eventType == "dismissed") {
        ev.type = rex::modules::ModuleEventType::NotificationDismissed;
    } else if (eventType == "action_invoked") {
        ev.type = rex::modules::ModuleEventType::NotificationActionInvoked;
    } else if (eventType == "history_cleared") {
        ev.type = rex::modules::ModuleEventType::HistoryCleared;
    } else {
        return;
    }

    bus_.publish(ev);
}

}
