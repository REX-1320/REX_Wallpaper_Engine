#include "NotificationSound.hpp"

namespace rex::notifications {

void NotificationSound::playForNotification(const Notification& notification) {
    (void)notification;
}

void NotificationSound::setSoundFile(NotificationType type, const std::string& filePath) {
    soundFiles_[type] = filePath;
}

}
