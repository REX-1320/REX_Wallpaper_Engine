#pragma once
#include <vector>
#include <string>
#include "../registry/SettingsRegistry.hpp"
#include "../models/SearchResult.hpp"

namespace rex::settings {

class SettingsSearch {
public:
    explicit SettingsSearch(const SettingsRegistry& registry);
    std::vector<SearchResult> search(const std::string& query,
                                     const std::string& category = "",
                                     int maxResults = 20) const;
private:
    const SettingsRegistry& registry_;
    double relevance(const Setting& setting, const std::string& lowerQuery) const;
};

}
