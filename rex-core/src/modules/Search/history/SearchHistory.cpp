#include "SearchHistory.hpp"
#include <algorithm>

namespace rex::search {

void SearchHistory::add(const std::string& query) {
    std::lock_guard lock(mutex_);
    auto it = std::find(recent_.begin(), recent_.end(), query);
    if (it != recent_.end()) recent_.erase(it);
    recent_.insert(recent_.begin(), query);
    if (recent_.size() > MaxEntries) recent_.pop_back();
}

std::vector<std::string> SearchHistory::getRecent(int maxCount) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result = recent_;
    if (result.size() > static_cast<size_t>(maxCount))
        result.resize(maxCount);
    return result;
}

void SearchHistory::clear() {
    std::lock_guard lock(mutex_);
    recent_.clear();
}

}
