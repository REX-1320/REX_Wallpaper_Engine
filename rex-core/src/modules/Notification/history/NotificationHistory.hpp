#pragma once
#include <vector>
#include <string>
#include <mutex>
#include "../models/Notification.hpp"

namespace rex::notifications {

class NotificationHistory {
public:
    void add(const Notification& notification);

    std::vector<Notification> getAll(const std::string& moduleFilter = "",
                                     NotificationType typeFilter = NotificationType::Custom) const;

    void clear();

    std::vector<Notification> search(const std::string& query) const;

    void setMaxSize(size_t max) { maxSize_ = max; }
    size_t maxSize() const { return maxSize_; }

private:
    mutable std::mutex mutex_;
    std::vector<Notification> entries_;
    size_t maxSize_ = 1000;
};

}
