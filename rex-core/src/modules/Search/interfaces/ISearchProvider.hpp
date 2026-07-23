#pragma once
#include <vector>
#include <string>
#include "../models/SearchQuery.hpp"
#include "../models/SearchResult.hpp"
#include "../models/SearchProviderMetadata.hpp"
#include "../models/ICancellationToken.hpp"

namespace rex::search {

class ISearchProvider {
public:
    virtual ~ISearchProvider() = default;

    virtual SearchProviderMetadata getMetadata() const = 0;
    virtual std::vector<SearchResult> search(const SearchQuery& query,
                                             ICancellationToken* cancelToken = nullptr) = 0;

    virtual std::vector<std::string> getKeywords() const { return {}; }

    virtual bool isAvailable() const { return true; }
};

}
