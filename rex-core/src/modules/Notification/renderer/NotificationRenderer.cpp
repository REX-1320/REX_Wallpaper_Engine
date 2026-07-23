#include "NotificationRenderer.hpp"
#include <algorithm>

namespace rex::notifications {

class NotificationCard {
public:
    std::string id;
    Notification data;
    float alpha = 0.0f;
    float slideOffset = 50.0f;
};

NotificationRenderer::NotificationRenderer() = default;
NotificationRenderer::~NotificationRenderer() = default;

void NotificationRenderer::show(const Notification& notification) {
    auto card = std::make_unique<NotificationCard>();
    card->id = notification.id;
    card->data = notification;
    card->alpha = 0.0f;
    card->slideOffset = 50.0f;
    cards_[notification.id] = std::move(card);
}

void NotificationRenderer::update(const Notification& notification) {
    auto it = cards_.find(notification.id);
    if (it != cards_.end()) {
        it->second->data = notification;
    }
}

void NotificationRenderer::dismiss(const std::string& notificationId, std::function<void()> onFinished) {
    auto it = cards_.find(notificationId);
    if (it != cards_.end()) {
        cards_.erase(it);
        if (onFinished) onFinished();
    }
}

void NotificationRenderer::clearAll() {
    cards_.clear();
}

void NotificationRenderer::setPosition(int screenEdge, int offsetX, int offsetY) {
    edge_ = screenEdge;
    offsetX_ = offsetX;
    offsetY_ = offsetY;
}

void NotificationRenderer::tick(float deltaTime) {
    for (auto& [id, card] : cards_) {
        if (card->alpha < 1.0f) {
            card->alpha += deltaTime * 4.0f;
            if (card->alpha > 1.0f) card->alpha = 1.0f;
        }
        if (card->slideOffset > 0.0f) {
            card->slideOffset -= deltaTime * 120.0f;
            if (card->slideOffset < 0.0f) card->slideOffset = 0.0f;
        }
    }
}

}
