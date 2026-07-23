#pragma once
#include <vector>
#include <string>
#include <mutex>

namespace rex::search {

class SearchHistory {
public:
    void add(const std::string& query);
    std::vector<std::string> getRecent(int maxCount) const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::string> recent_;
    static constexpr size_t MaxEntries = 100;
};

}
