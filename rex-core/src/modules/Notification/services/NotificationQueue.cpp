#include "NotificationQueue.hpp"
#include <algorithm>

namespace rex::notifications {

size_t NotificationQueue::enqueue(const Notification& notification) {
    std::lock_guard lock(mutex_);
    queue_.push_back(notification);
    sortByPriority();
    return queue_.size() - 1;
}

std::optional<Notification> NotificationQueue::dequeue() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    Notification n = queue_.front();
    queue_.pop_front();
    return n;
}

const Notification* NotificationQueue::peek() const {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return nullptr;
    return &queue_.front();
}

bool NotificationQueue::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Notification& n) {
        return n.id == id;
    });
    if (it != queue_.end()) {
        queue_.erase(it);
        return true;
    }
    return false;
}

void NotificationQueue::prioritize(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Notification& n) {
        return n.id == id;
    });
    if (it != queue_.end()) {
        Notification n = *it;
        queue_.erase(it);
        queue_.push_front(n);
    }
}

void NotificationQueue::clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
}

size_t NotificationQueue::size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void NotificationQueue::sortByPriority() {
    std::stable_sort(queue_.begin(), queue_.end(), [](const Notification& a, const Notification& b) {
        auto weight = [](NotificationPriority p) {
            switch (p) {
                case NotificationPriority::Critical: return 5;
                case NotificationPriority::High:     return 4;
                case NotificationPriority::Normal:   return 2;
                case NotificationPriority::Low:      return 1;
                case NotificationPriority::Background: return 0;
                default: return 2;
            }
        };
        return weight(a.priority) > weight(b.priority);
    });
}

}
