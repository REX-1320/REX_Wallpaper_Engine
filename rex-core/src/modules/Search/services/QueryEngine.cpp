#include "QueryEngine.hpp"
#include "ProviderRegistry.hpp"
#include "RankingEngine.hpp"
#include "../index/IndexCoordinator.hpp"
#include "../semantic/SemanticIndex.hpp"
#include <future>
#include <chrono>
#include <algorithm>

namespace rex::search {

QueryEngine::QueryEngine(ProviderRegistry& registry, RankingEngine& ranking,
                         IndexCoordinator* sharedIndex)
    : registry_(registry), ranking_(ranking), sharedIndex_(sharedIndex) {}

void QueryEngine::updateHealth(const std::string& providerId, const ProviderHealth& health) {
    std::lock_guard lock(healthMutex_);
    health_[providerId] = health;
}

ProviderHealth QueryEngine::getHealth(const std::string& providerId) const {
    std::lock_guard lock(healthMutex_);
    auto it = health_.find(providerId);
    if (it != health_.end()) return it->second;
    ProviderHealth h;
    h.providerId = providerId;
    h.status = "unknown";
    return h;
}

std::vector<ProviderHealth> QueryEngine::getAllHealth() const {
    std::lock_guard lock(healthMutex_);
    std::vector<ProviderHealth> result;
    for (auto& [id, h] : health_) result.push_back(h);
    return result;
}

std::vector<SearchResult> QueryEngine::execute(const SearchQuery& query,
                                               std::shared_ptr<ICancellationToken> cancelToken) {
    auto providers = registry_.getAll();

    providers.erase(std::remove_if(providers.begin(), providers.end(), [&](ISearchProvider* p) {
        if (!p->isAvailable()) return true;
        if (query.scope != SearchScope::Global) {
            auto cats = p->getMetadata().categories;
            bool found = false;
            for (auto& cat : cats) {
                if (cat == "Settings" && query.scope == SearchScope::Settings) found = true;
                else if (cat == "Commands" && query.scope == SearchScope::Commands) found = true;
                else if (cat == "Wallpapers" && query.scope == SearchScope::Wallpapers) found = true;
                else if (cat == "Widgets" && query.scope == SearchScope::Widgets) found = true;
                else if (cat == "Marketplace" && query.scope == SearchScope::Marketplace) found = true;
                else if (cat == "Developer" && query.scope == SearchScope::Developer) found = true;
            }
            if (!found) return true;
        }
        return false;
    }), providers.end());

    std::vector<std::future<std::vector<SearchResult>>> futures;
    for (auto* provider : providers) {
        int timeoutMs = provider->getMetadata().timeoutMs;
        futures.push_back(std::async(std::launch::async, [provider, query, cancelToken, timeoutMs]() {
            if (cancelToken && cancelToken->isCancelled()) return std::vector<SearchResult>{};
            return provider->search(query, cancelToken.get());
        }));
    }

    std::vector<SearchResult> allResults;
    for (size_t i = 0; i < futures.size(); ++i) {
        if (cancelToken && cancelToken->isCancelled()) break;
        auto status = futures[i].wait_for(std::chrono::milliseconds(providers[i]->getMetadata().timeoutMs));
        if (status == std::future_status::ready) {
            auto res = futures[i].get();
            allResults.insert(allResults.end(), res.begin(), res.end());
        }
    }

    if (sharedIndex_ && !query.tokens.empty()) {
        auto indexHits = sharedIndex_->search(query.tokens);
        for (auto& [docId, score] : indexHits) {
            SearchResult r;
            r.id = docId;
            r.title = docId;
            r.relevanceScore = score / 10.0;
            r.timestamp = std::chrono::system_clock::now();
            allResults.push_back(r);
        }
    }

    // Hybrid: enrich with semantic scores
    std::unordered_map<std::string, double> semanticScores;
    if (semanticIndex_ && !query.rawText.empty()) {
        auto semanticHits = semanticIndex_->search(query.rawText, 50);
        for (auto& hit : semanticHits) {
            semanticScores[hit.documentId] = hit.score;
            // Add semantic-only results that have stored SearchResults
            bool alreadyInResults = false;
            for (auto& r : allResults) {
                if ((r.providerId + ":" + r.id) == hit.documentId) {
                    alreadyInResults = true;
                    break;
                }
            }
            if (!alreadyInResults) {
                auto doc = semanticIndex_->getDocument(hit.documentId);
                if (doc.has_value()) {
                    SearchResult sr = doc.value();
                    sr.relevanceScore = hit.score;
                    allResults.push_back(sr);
                }
            }
        }
        ranking_.setSemanticScores(semanticScores);
    }

    allResults.erase(std::remove_if(allResults.begin(), allResults.end(), [&](const SearchResult& r) {
        for (auto& filter : query.filters) {
            if (filter.type == SearchFilter::Type::Category && r.category != filter.value) return true;
        }
        return false;
    }), allResults.end());

    ranking_.rank(allResults, query);

    if (query.offset > 0) {
        if (query.offset < allResults.size())
            allResults.erase(allResults.begin(), allResults.begin() + query.offset);
        else
            allResults.clear();
    }
    if (allResults.size() > static_cast<size_t>(query.maxResults))
        allResults.resize(query.maxResults);

    return allResults;
}

}
