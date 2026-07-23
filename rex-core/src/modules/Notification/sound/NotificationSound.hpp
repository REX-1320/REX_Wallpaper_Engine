#pragma once
#include <string>
#include <unordered_map>
#include "../models/Notification.hpp"

namespace rex::notifications {

class NotificationSound {
public:
    void playForNotification(const Notification& notification);

    void setSoundFile(NotificationType type, const std::string& filePath);

    void setMuted(bool muted) { muted_ = muted; }

private:
    std::unordered_map<NotificationType, std::string> soundFiles_;
    bool muted_ = false;
};

}
