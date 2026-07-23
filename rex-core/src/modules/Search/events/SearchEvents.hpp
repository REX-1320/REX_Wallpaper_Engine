#pragma once
#include <string>

namespace rex::search {

struct SearchQueryExecutedEvent {
    std::string queryText;
    int resultCount;
    int durationMs;
};

struct SearchFailedEvent {
    std::string errorMessage;
};

struct ProviderRegisteredEvent {
    std::string providerId;
};

struct ProviderUnregisteredEvent {
    std::string providerId;
};

struct ProviderTimeoutEvent {
    std::string providerId;
    std::string query;
};

struct SuggestionRequestedEvent {
    std::string partialText;
};

struct HistoryClearedEvent {};

}
