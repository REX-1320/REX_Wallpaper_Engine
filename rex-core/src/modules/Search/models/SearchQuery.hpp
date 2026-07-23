#pragma once
#include <string>
#include <vector>
#include <any>
#include "SearchFilter.hpp"
#include "SearchScope.hpp"

namespace rex::search {
struct SearchContext {
    std::string currentModule;
    std::string currentView;
    std::string focusedObjectId;
    std::any selectedItem;
};

struct SearchQuery {
    std::string rawText;
    std::vector<std::string> tokens;
    std::vector<SearchFilter> filters;
    SearchScope scope = SearchScope::Global;
    SearchContext context;
    int maxResults = 20;
    int offset = 0;
    bool includeSuggestions = false;
};
}
