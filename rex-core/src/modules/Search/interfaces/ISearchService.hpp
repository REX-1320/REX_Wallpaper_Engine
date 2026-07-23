#pragma once
#include <vector>
#include <string>
#include <memory>
#include "../models/SearchQuery.hpp"
#include "../models/SearchResult.hpp"
#include "../models/Suggestion.hpp"
#include "../models/SearchProviderMetadata.hpp"
#include "../models/ICancellationToken.hpp"

namespace rex::search {

class ISearchProvider;

class ISearchService {
public:
    virtual ~ISearchService() = default;

    virtual bool registerProvider(ISearchProvider* provider) = 0;
    virtual bool unregisterProvider(const std::string& providerId) = 0;

    virtual std::vector<SearchResult> search(const SearchQuery& query) = 0;

    virtual std::vector<SearchResult> searchWithCancellation(const SearchQuery& query,
                                                             std::shared_ptr<ICancellationToken> token) = 0;

    virtual std::vector<Suggestion> getSuggestions(const std::string& partialText,
                                                   int maxResults = 5) = 0;

    virtual void addToHistory(const std::string& queryText) = 0;
    virtual std::vector<std::string> getRecentQueries(int maxCount = 10) = 0;
    virtual void clearHistory() = 0;

    virtual std::vector<SearchProviderMetadata> getProviders() = 0;
};

}
