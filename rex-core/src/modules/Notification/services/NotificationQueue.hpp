#pragma once
#include <deque>
#include <mutex>
#include <optional>
#include "../models/Notification.hpp"

namespace rex::notifications {

class NotificationQueue {
public:
    size_t enqueue(const Notification& notification);
    std::optional<Notification> dequeue();
    const Notification* peek() const;
    bool remove(const std::string& id);
    void prioritize(const std::string& id);
    void clear();
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<Notification> queue_;
    void sortByPriority();
};

}
