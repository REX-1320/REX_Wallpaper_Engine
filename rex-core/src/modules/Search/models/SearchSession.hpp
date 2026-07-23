#pragma once
#include <string>
#include <chrono>
#include <vector>
#include "SearchQuery.hpp"
#include "SearchResult.hpp"

namespace rex::search {

struct SearchSession {
    std::string sessionId;
    SearchQuery originalQuery;
    std::chrono::steady_clock::time_point startTime;
    std::vector<SearchResult> results;
    std::string clickedResultId;
    bool cancelled = false;
};
}
