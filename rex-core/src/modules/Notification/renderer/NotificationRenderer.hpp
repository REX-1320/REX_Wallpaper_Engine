#pragma once
#include "INotificationRenderer.hpp"
#include <unordered_map>
#include <memory>

namespace rex::notifications {

class NotificationCard;

class NotificationRenderer : public INotificationRenderer {
public:
    NotificationRenderer();
    ~NotificationRenderer() override;

    void show(const Notification& notification) override;
    void update(const Notification& notification) override;
    void dismiss(const std::string& notificationId, std::function<void()> onFinished) override;
    void clearAll() override;
    void setPosition(int screenEdge, int offsetX, int offsetY) override;
    void tick(float deltaTime) override;

private:
    std::unordered_map<std::string, std::unique_ptr<NotificationCard>> cards_;
    int edge_ = 1;
    int offsetX_ = 20, offsetY_ = 20;
};

}
