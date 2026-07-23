#pragma once
#include "../interfaces/ISearchService.hpp"
#include "ProviderRegistry.hpp"
#include "QueryEngine.hpp"
#include "RankingEngine.hpp"
#include "SuggestionService.hpp"
#include "MetricsCollector.hpp"
#include "../history/SearchHistory.hpp"
#include "../models/SearchSession.hpp"
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace rex::modules { class EventBus; }
namespace rex::search::semantic { class SemanticIndex; }

namespace rex::search {

class SearchService : public ISearchService {
public:
    SearchService(std::unique_ptr<ProviderRegistry> registry,
                  std::unique_ptr<QueryEngine> queryEngine,
                  std::unique_ptr<RankingEngine> rankingEngine,
                  std::unique_ptr<SuggestionService> suggestionService,
                  std::unique_ptr<SearchHistory> history,
                  rex::modules::EventBus& bus);

    bool registerProvider(ISearchProvider* provider) override;
    bool unregisterProvider(const std::string& providerId) override;
    std::vector<SearchResult> search(const SearchQuery& query) override;
    std::vector<SearchResult> searchWithCancellation(const SearchQuery& query,
                                                     std::shared_ptr<ICancellationToken> token) override;
    std::vector<Suggestion> getSuggestions(const std::string& partialText, int maxResults = 5) override;
    void addToHistory(const std::string& queryText) override;
    std::vector<std::string> getRecentQueries(int maxCount = 10) override;
    void clearHistory() override;
    std::vector<SearchProviderMetadata> getProviders() override;

    int getQueryCount() const { return queryCount_.load(); }

    // Phase 2: Sessions
    std::string startSession(const SearchQuery& query);
    void recordClick(const std::string& sessionId, const std::string& resultId);
    void cancelSession(const std::string& sessionId);
    SearchSession getSession(const std::string& sessionId) const;
    std::vector<SearchSession> getAllSessions() const;

    // Phase 2: Metrics
    SearchMetrics getMetrics() const;
    void resetMetrics();

    // Phase 2: Health
    void updateHealth(const std::string& providerId, const ProviderHealth& health);
    ProviderHealth getHealth(const std::string& providerId) const;
    std::vector<ProviderHealth> getAllHealth() const;

    // Semantic
    void setSemanticIndex(std::shared_ptr<semantic::SemanticIndex> index);
    std::shared_ptr<semantic::SemanticIndex> getSemanticIndex() const { return semanticIndex_; }

private:
    void publishEvent(const std::string& eventType, const std::string& data);

    std::unique_ptr<ProviderRegistry> registry_;
    std::unique_ptr<QueryEngine> queryEngine_;
    std::unique_ptr<RankingEngine> rankingEngine_;
    std::unique_ptr<SuggestionService> suggestionService_;
    std::unique_ptr<SearchHistory> history_;
    std::unique_ptr<MetricsCollector> metrics_;
    std::shared_ptr<semantic::SemanticIndex> semanticIndex_;
    rex::modules::EventBus& bus_;
    std::atomic<int> queryCount_{0};

    // Sessions
    std::unordered_map<std::string, SearchSession> sessions_;
    mutable std::mutex sessionMutex_;
    int sessionCounter_ = 0;
};

}
