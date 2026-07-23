#pragma once
#include <string>
#include <vector>
#include <optional>
#include "../models/Notification.hpp"

namespace rex::notifications {

class INotificationService {
public:
    virtual ~INotificationService() = default;

    virtual std::string show(const Notification& notification) = 0;
    virtual bool update(const std::string& id, const Notification& updated) = 0;
    virtual void dismiss(const std::string& id) = 0;
    virtual void dismissAll() = 0;
    virtual std::vector<Notification> getDisplayed() = 0;
    virtual std::optional<Notification> getById(const std::string& id) = 0;

    virtual void invokeAction(const std::string& notificationId, const std::string& actionId) = 0;
    virtual std::vector<Notification> getHistory(const std::string& moduleFilter = "",
                                                 NotificationType typeFilter = NotificationType::Custom) = 0;
    virtual void clearHistory() = 0;
};

}
