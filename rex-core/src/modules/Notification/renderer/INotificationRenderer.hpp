#pragma once
#include <string>
#include <functional>
#include "../models/Notification.hpp"

namespace rex::notifications {

class INotificationRenderer {
public:
    virtual ~INotificationRenderer() = default;

    virtual void show(const Notification& notification) = 0;
    virtual void update(const Notification& notification) = 0;
    virtual void dismiss(const std::string& notificationId,
                         std::function<void()> onFinished = nullptr) = 0;
    virtual void clearAll() = 0;
    virtual void setPosition(int screenEdge, int offsetX, int offsetY) = 0;
    virtual void tick(float deltaTime) = 0;
};

}
