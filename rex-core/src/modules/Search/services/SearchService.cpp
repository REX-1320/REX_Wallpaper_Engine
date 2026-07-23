#include "SearchService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include "ProviderRegistry.hpp"
#include "../interfaces/ISemanticSearchProvider.hpp"
#include "../semantic/SemanticIndex.hpp"
#include <chrono>
#include <sstream>

namespace rex::search {

SearchService::SearchService(std::unique_ptr<ProviderRegistry> registry,
                             std::unique_ptr<QueryEngine> queryEngine,
                             std::unique_ptr<RankingEngine> rankingEngine,
                             std::unique_ptr<SuggestionService> suggestionService,
                             std::unique_ptr<SearchHistory> history,
                             rex::modules::EventBus& bus)
    : registry_(std::move(registry))
    , queryEngine_(std::move(queryEngine))
    , rankingEngine_(std::move(rankingEngine))
    , suggestionService_(std::move(suggestionService))
    , history_(std::move(history))
    , metrics_(std::make_unique<MetricsCollector>())
    , bus_(bus) {}

void SearchService::publishEvent(const std::string& eventType, const std::string& data) {
    rex::modules::ModuleEvent ev;
    ev.moduleId = "rex.search";
    ev.error = eventType + ":" + data;
    ev.type = rex::modules::ModuleEventType::SearchQueryExecuted;
    bus_.publish(ev);
}

bool SearchService::registerProvider(ISearchProvider* provider) {
    if (registry_->add(provider)) {
        publishEvent("provider_registered", provider->getMetadata().id);
        if (semanticIndex_) {
            if (auto* semProvider = dynamic_cast<semantic::ISemanticSearchProvider*>(provider)) {
                semProvider->indexDocuments(
                    [this, providerId = provider->getMetadata().id](
                        const std::string& pid, const std::string& lid, const std::string& text) {
                        SearchResult r;
                        r.id = lid;
                        r.providerId = pid;
                        r.title = text;
                        semanticIndex_->indexDocument(pid, lid, text, r);
                    });
            }
        }
        return true;
    }
    return false;
}

bool SearchService::unregisterProvider(const std::string& providerId) {
    if (registry_->remove(providerId)) {
        publishEvent("provider_unregistered", providerId);
        return true;
    }
    return false;
}

std::vector<SearchResult> SearchService::search(const SearchQuery& query) {
    queryCount_++;
    auto start = std::chrono::steady_clock::now();
    auto results = queryEngine_->execute(query);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (metrics_) {
        for (auto* p : registry_->getAll()) {
            metrics_->recordQuery(p->getMetadata().id, std::chrono::milliseconds(duration));
        }
    }

    publishEvent("search_executed", query.rawText + "|" + std::to_string(results.size()) + "|" + std::to_string(duration));
    return results;
}

std::vector<SearchResult> SearchService::searchWithCancellation(const SearchQuery& query,
                                                                std::shared_ptr<ICancellationToken> token) {
    queryCount_++;
    auto results = queryEngine_->execute(query, token);
    return results;
}

std::vector<Suggestion> SearchService::getSuggestions(const std::string& partialText, int maxResults) {
    return suggestionService_->getSuggestions(partialText, maxResults);
}

void SearchService::addToHistory(const std::string& queryText) {
    history_->add(queryText);
}

std::vector<std::string> SearchService::getRecentQueries(int maxCount) {
    return history_->getRecent(maxCount);
}

void SearchService::clearHistory() {
    history_->clear();
    publishEvent("history_cleared", "");
}

std::vector<SearchProviderMetadata> SearchService::getProviders() {
    std::vector<SearchProviderMetadata> result;
    for (auto* p : registry_->getAll()) {
        result.push_back(p->getMetadata());
    }
    return result;
}

// ── Semantic ─────────────────────────────────────────────────────────────────

void SearchService::setSemanticIndex(std::shared_ptr<semantic::SemanticIndex> index) {
    semanticIndex_ = index;
    queryEngine_->setSemanticIndex(index);
    rankingEngine_->setSemanticIndex(index.get());
}

// ── Session Management ──────────────────────────────────────────────────────

std::string SearchService::startSession(const SearchQuery& query) {
    std::lock_guard lock(sessionMutex_);
    std::ostringstream ss;
    ss << "session_" << (++sessionCounter_);
    SearchSession session;
    session.sessionId = ss.str();
    session.originalQuery = query;
    session.startTime = std::chrono::steady_clock::now();
    sessions_[session.sessionId] = session;
    return session.sessionId;
}

void SearchService::recordClick(const std::string& sessionId, const std::string& resultId) {
    std::lock_guard lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        it->second.clickedResultId = resultId;
    }
}

void SearchService::cancelSession(const std::string& sessionId) {
    std::lock_guard lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        it->second.cancelled = true;
    }
}

SearchSession SearchService::getSession(const std::string& sessionId) const {
    std::lock_guard lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) return it->second;
    return {};
}

std::vector<SearchSession> SearchService::getAllSessions() const {
    std::lock_guard lock(sessionMutex_);
    std::vector<SearchSession> result;
    for (auto& [id, s] : sessions_) result.push_back(s);
    return result;
}

// ── Metrics ─────────────────────────────────────────────────────────────────

SearchMetrics SearchService::getMetrics() const {
    if (metrics_) return metrics_->getMetrics();
    return {};
}

void SearchService::resetMetrics() {
    if (metrics_) metrics_->reset();
}

// ── Health ───────────────────────────────────────────────────────────────────

void SearchService::updateHealth(const std::string& providerId, const ProviderHealth& health) {
    queryEngine_->updateHealth(providerId, health);
}

ProviderHealth SearchService::getHealth(const std::string& providerId) const {
    return queryEngine_->getHealth(providerId);
}

std::vector<ProviderHealth> SearchService::getAllHealth() const {
    return queryEngine_->getAllHealth();
}

}
