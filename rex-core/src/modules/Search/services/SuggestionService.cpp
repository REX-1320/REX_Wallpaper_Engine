#include "SuggestionService.hpp"
#include "../history/SearchHistory.hpp"
#include "ProviderRegistry.hpp"
#include <algorithm>

namespace rex::search {

SuggestionService::SuggestionService(SearchHistory& history, ProviderRegistry& registry)
    : history_(history), registry_(registry) {}

std::vector<Suggestion> SuggestionService::getSuggestions(const std::string& partialText, int maxResults) {
    std::vector<Suggestion> suggestions;
    std::string lower = partialText;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (auto& hist : history_.getRecent(20)) {
        std::string hlower = hist;
        std::transform(hlower.begin(), hlower.end(), hlower.begin(), ::tolower);
        if (hlower.find(lower) != std::string::npos) {
            suggestions.push_back({hist, "history", 0.8});
        }
    }

    for (auto* provider : registry_.getAll()) {
        for (auto& kw : provider->getKeywords()) {
            std::string klower = kw;
            std::transform(klower.begin(), klower.end(), klower.begin(), ::tolower);
            if (klower.find(lower) != std::string::npos) {
                suggestions.push_back({kw, provider->getMetadata().displayName, 0.6});
            }
        }
    }

    std::sort(suggestions.begin(), suggestions.end(),
              [](const Suggestion& a, const Suggestion& b) { return a.weight > b.weight; });
    if (suggestions.size() > static_cast<size_t>(maxResults))
        suggestions.resize(maxResults);
    return suggestions;
}

}
