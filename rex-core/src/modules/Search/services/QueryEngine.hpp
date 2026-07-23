#pragma once
#include <vector>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include "../models/SearchQuery.hpp"
#include "../models/SearchResult.hpp"
#include "../models/ICancellationToken.hpp"
#include "../models/ProviderHealth.hpp"

namespace rex::search {

class ProviderRegistry;
class RankingEngine;
class IndexCoordinator;
namespace semantic { class SemanticIndex; }

class QueryEngine {
public:
    QueryEngine(ProviderRegistry& registry, RankingEngine& ranking,
                IndexCoordinator* sharedIndex = nullptr);

    void setSemanticIndex(std::shared_ptr<semantic::SemanticIndex> index) { semanticIndex_ = index; }
    std::shared_ptr<semantic::SemanticIndex> getSemanticIndex() const { return semanticIndex_; }

    std::vector<SearchResult> execute(const SearchQuery& query,
                                      std::shared_ptr<ICancellationToken> cancelToken = nullptr);

    void updateHealth(const std::string& providerId, const ProviderHealth& health);
    ProviderHealth getHealth(const std::string& providerId) const;
    std::vector<ProviderHealth> getAllHealth() const;

private:
    ProviderRegistry& registry_;
    RankingEngine& ranking_;
    IndexCoordinator* sharedIndex_;
    std::shared_ptr<semantic::SemanticIndex> semanticIndex_;
    std::unordered_map<std::string, ProviderHealth> health_;
    mutable std::mutex healthMutex_;
};

}
