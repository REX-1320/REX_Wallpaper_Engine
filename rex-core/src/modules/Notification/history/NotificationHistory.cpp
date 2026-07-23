#include "NotificationHistory.hpp"
#include <algorithm>
#include <cctype>

namespace rex::notifications {

void NotificationHistory::add(const Notification& notification) {
    std::lock_guard lock(mutex_);
    entries_.push_back(notification);
    if (maxSize_ > 0 && entries_.size() > maxSize_) {
        entries_.erase(entries_.begin());
    }
}

std::vector<Notification> NotificationHistory::getAll(const std::string& moduleFilter,
                                                      NotificationType typeFilter) const {
    std::lock_guard lock(mutex_);
    std::vector<Notification> result;
    for (const auto& n : entries_) {
        if (!moduleFilter.empty() && n.moduleId != moduleFilter) continue;
        if (typeFilter != NotificationType::Custom && n.type != typeFilter) continue;
        result.push_back(n);
    }
    return result;
}

void NotificationHistory::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

static std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::vector<Notification> NotificationHistory::search(const std::string& query) const {
    std::lock_guard lock(mutex_);
    std::vector<Notification> result;
    std::string lowerQuery = toLower(query);
    for (const auto& n : entries_) {
        if (toLower(n.title).find(lowerQuery) != std::string::npos ||
            toLower(n.message).find(lowerQuery) != std::string::npos) {
            result.push_back(n);
        }
    }
    return result;
}

}
