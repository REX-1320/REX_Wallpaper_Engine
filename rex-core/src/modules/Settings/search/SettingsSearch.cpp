#include "SettingsSearch.hpp"
#include <algorithm>
#include <cctype>

namespace rex::settings {

static std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

SettingsSearch::SettingsSearch(const SettingsRegistry& registry)
    : registry_(registry) {}

double SettingsSearch::relevance(const Setting& s, const std::string& lowerQuery) const {
    double score = 0.0;
    if (toLower(s.name).find(lowerQuery) != std::string::npos) score += 10.0;
    if (toLower(s.description).find(lowerQuery) != std::string::npos) score += 5.0;
    if (toLower(s.id).find(lowerQuery) != std::string::npos) score += 8.0;
    if (toLower(s.category).find(lowerQuery) != std::string::npos) score += 3.0;
    return score;
}

std::vector<SearchResult> SettingsSearch::search(const std::string& query,
                                                 const std::string& category,
                                                 int maxResults) const {
    std::vector<SearchResult> results;
    std::string lowerQ = toLower(query);
    auto settings = registry_.getAllSettings();
    for (const auto& s : settings) {
        if (!category.empty() && s.category != category) continue;
        double r = relevance(s, lowerQ);
        if (r > 0.0) results.push_back({s, r});
    }
    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.relevance > b.relevance; });
    if (results.size() > static_cast<size_t>(maxResults))
        results.resize(maxResults);
    return results;
}

}
