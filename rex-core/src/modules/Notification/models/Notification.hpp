#pragma once
#include <string>
#include <vector>
#include <chrono>
#include "NotificationType.hpp"
#include "NotificationPriority.hpp"
#include "NotificationAction.hpp"
#include "NotificationState.hpp"

namespace rex::notifications {

struct Notification {
    std::string id;
    std::string moduleId;
    std::string title;
    std::string message;
    NotificationType type = NotificationType::Information;
    NotificationPriority priority = NotificationPriority::Normal;
    std::chrono::steady_clock::time_point creationTime;
    std::chrono::milliseconds duration{5000};
    std::string icon;
    std::vector<NotificationAction> actions;
    int progress = -1;
    std::string progressStatus;
    bool closable = true;
    bool persistent = false;
    bool silent = false;
    NotificationState state = NotificationState::Pending;
    std::string groupKey;
    int duplicateCount = 1;
    bool isGrouped = false;
};

}
