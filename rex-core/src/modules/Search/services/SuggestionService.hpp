#pragma once
#include <vector>
#include <string>
#include "../models/Suggestion.hpp"

namespace rex::search {

class SearchHistory;
class ProviderRegistry;

class SuggestionService {
public:
    SuggestionService(SearchHistory& history, ProviderRegistry& registry);

    std::vector<Suggestion> getSuggestions(const std::string& partialText, int maxResults);

private:
    SearchHistory& history_;
    ProviderRegistry& registry_;
};

}
