#include <gtest/gtest.h>
#include <memory>
#include <chrono>
#include <thread>
#include <algorithm>

#include "modules/Search/models/SearchScope.hpp"
#include "modules/Search/models/SearchCapabilities.hpp"
#include "modules/Search/models/SearchProviderMetadata.hpp"
#include "modules/Search/models/SearchFilter.hpp"
#include "modules/Search/models/SearchQuery.hpp"
#include "modules/Search/models/SearchResult.hpp"
#include "modules/Search/models/Suggestion.hpp"
#include "modules/Search/models/ICancellationToken.hpp"
#include "modules/Search/models/ProviderHealth.hpp"
#include "modules/Search/models/SearchSession.hpp"
#include "modules/Search/services/ProviderRegistry.hpp"
#include "modules/Search/services/RankingEngine.hpp"
#include "modules/Search/services/QueryEngine.hpp"
#include "modules/Search/services/SuggestionService.hpp"
#include "modules/Search/services/SearchService.hpp"
#include "modules/Search/services/MetricsCollector.hpp"
#include "modules/Search/history/SearchHistory.hpp"
#include "modules/Search/index/IndexCoordinator.hpp"
#include "modules/Search/semantic/EmbeddingService.hpp"
#include "modules/Search/semantic/SemanticIndex.hpp"
#include "modules/Search/interfaces/ISemanticSearchProvider.hpp"
#include "modules/Search/interfaces/ISearchProvider.hpp"
#include "modules/Search/interfaces/ISearchService.hpp"
#include "modules/Search/events/SearchEvents.hpp"
#include "modules/EventBus.hpp"

using namespace rex::search;
using namespace rex::modules;

// ── Mock Provider ──────────────────────────────────────────────────────────

class MockSearchProvider : public ISearchProvider {
public:
    explicit MockSearchProvider(const std::string& id,
                                const std::string& displayName = "",
                                int priority = 0,
                                SearchCapability caps = SearchCapability::None,
                                const std::vector<std::string>& cats = {},
                                const std::vector<std::string>& keywords = {})
        : metadata_{id, displayName, "", cats, priority, 500, caps}
        , keywords_(keywords) {}

    SearchProviderMetadata getMetadata() const override { return metadata_; }

    std::vector<SearchResult> search(const SearchQuery& query,
                                     ICancellationToken* cancelToken = nullptr) override {
        if (cancelToken && cancelToken->isCancelled()) return {};

        // Return canned results
        std::vector<SearchResult> results;
        for (auto& r : cannedResults_) {
            SearchResult cr = r;
            cr.providerId = metadata_.id;
            results.push_back(cr);
        }
        return results;
    }

    std::vector<std::string> getKeywords() const override { return keywords_; }

    void addCannedResult(const SearchResult& r) { cannedResults_.push_back(r); }
    void clearCannedResults() { cannedResults_.clear(); }

    SearchProviderMetadata metadata_;

private:
    std::vector<std::string> keywords_;
    std::vector<SearchResult> cannedResults_;
};

// ── SearchScope Tests ─────────────────────────────────────────────────

TEST(SearchScopeTest, EnumHasValues) {
    EXPECT_EQ(static_cast<int>(SearchScope::Global), 0);
    EXPECT_EQ(static_cast<int>(SearchScope::Settings), 1);
    EXPECT_EQ(static_cast<int>(SearchScope::Commands), 2);
    EXPECT_EQ(static_cast<int>(SearchScope::Wallpapers), 3);
    EXPECT_EQ(static_cast<int>(SearchScope::Widgets), 4);
    EXPECT_EQ(static_cast<int>(SearchScope::Marketplace), 5);
    EXPECT_EQ(static_cast<int>(SearchScope::Developer), 6);
    EXPECT_EQ(static_cast<int>(SearchScope::Custom), 7);
}

// ── SearchCapabilities Tests ──────────────────────────────────────────

TEST(SearchCapabilitiesTest, FlagCombination) {
    auto combined = SearchCapability::Fuzzy | SearchCapability::Regex;
    EXPECT_TRUE(hasCapability(combined, SearchCapability::Fuzzy));
    EXPECT_TRUE(hasCapability(combined, SearchCapability::Regex));
    EXPECT_FALSE(hasCapability(combined, SearchCapability::Tags));
}

TEST(SearchCapabilitiesTest, NoneHasNoFlags) {
    EXPECT_FALSE(hasCapability(SearchCapability::None, SearchCapability::Fuzzy));
    EXPECT_FALSE(hasCapability(SearchCapability::None, SearchCapability::Regex));
}

TEST(SearchCapabilitiesTest, SingleFlag) {
    EXPECT_TRUE(hasCapability(SearchCapability::Tags, SearchCapability::Tags));
    EXPECT_TRUE(hasCapability(SearchCapability::Semantic, SearchCapability::Semantic));
}

TEST(SearchCapabilitiesTest, MultipleFlagsThroughOr) {
    auto combined = SearchCapability::Prefix | SearchCapability::Boolean;
    EXPECT_TRUE(hasCapability(combined, SearchCapability::Prefix));
    EXPECT_TRUE(hasCapability(combined, SearchCapability::Boolean));
}

// ── SearchProviderMetadata Tests ──────────────────────────────────────

TEST(SearchProviderMetadataTest, DefaultConstruction) {
    SearchProviderMetadata m;
    EXPECT_TRUE(m.id.empty());
    EXPECT_TRUE(m.displayName.empty());
    EXPECT_EQ(m.priority, 0);
    EXPECT_EQ(m.timeoutMs, 500);
    EXPECT_EQ(m.capabilities, SearchCapability::None);
}

TEST(SearchProviderMetadataTest, CustomValues) {
    SearchProviderMetadata m;
    m.id = "test.provider";
    m.displayName = "Test Provider";
    m.priority = 10;
    m.timeoutMs = 1000;
    m.capabilities = SearchCapability::Fuzzy | SearchCapability::Tags;
    EXPECT_EQ(m.id, "test.provider");
    EXPECT_EQ(m.displayName, "Test Provider");
    EXPECT_EQ(m.priority, 10);
    EXPECT_EQ(m.timeoutMs, 1000);
    EXPECT_TRUE(hasCapability(m.capabilities, SearchCapability::Fuzzy));
    EXPECT_TRUE(hasCapability(m.capabilities, SearchCapability::Tags));
}

// ── SearchFilter Tests ────────────────────────────────────────────────

TEST(SearchFilterTest, TypeEnum) {
    EXPECT_NE(static_cast<int>(SearchFilter::Type::Category),
              static_cast<int>(SearchFilter::Type::Custom));
}

TEST(SearchFilterTest, CategoryFilter) {
    SearchFilter f;
    f.type = SearchFilter::Type::Category;
    f.value = "Settings";
    EXPECT_EQ(f.type, SearchFilter::Type::Category);
    EXPECT_EQ(f.value, "Settings");
}

// ── SearchQuery & SearchContext Tests ─────────────────────────────────

TEST(SearchQueryTest, DefaultConstruction) {
    SearchQuery q;
    EXPECT_TRUE(q.rawText.empty());
    EXPECT_TRUE(q.tokens.empty());
    EXPECT_EQ(q.scope, SearchScope::Global);
    EXPECT_EQ(q.maxResults, 20);
    EXPECT_EQ(q.offset, 0);
}

TEST(SearchQueryTest, CustomQuery) {
    SearchQuery q;
    q.rawText = "dark mode";
    q.tokens = {"dark", "mode"};
    q.scope = SearchScope::Settings;
    q.maxResults = 10;
    q.offset = 5;
    q.includeSuggestions = true;
    EXPECT_EQ(q.rawText, "dark mode");
    EXPECT_EQ(q.tokens.size(), 2u);
    EXPECT_EQ(q.scope, SearchScope::Settings);
    EXPECT_EQ(q.maxResults, 10);
    EXPECT_EQ(q.offset, 5);
    EXPECT_TRUE(q.includeSuggestions);
}

TEST(SearchContextTest, DefaultContext) {
    SearchContext ctx;
    EXPECT_TRUE(ctx.currentModule.empty());
    EXPECT_TRUE(ctx.currentView.empty());
}

TEST(SearchContextTest, CustomContext) {
    SearchContext ctx;
    ctx.currentModule = "test_module";
    ctx.currentView = "main_view";
    ctx.focusedObjectId = "obj_1";
    EXPECT_EQ(ctx.currentModule, "test_module");
    EXPECT_EQ(ctx.currentView, "main_view");
    EXPECT_EQ(ctx.focusedObjectId, "obj_1");
}

// ── SearchResult Tests ────────────────────────────────────────────────

TEST(SearchResultTest, DefaultConstruction) {
    SearchResult r;
    EXPECT_TRUE(r.id.empty());
    EXPECT_TRUE(r.providerId.empty());
    EXPECT_EQ(r.relevanceScore, 0.0);
    EXPECT_TRUE(r.tags.empty());
}

TEST(SearchResultTest, CustomResult) {
    SearchResult r;
    r.id = "res_1";
    r.title = "Dark Mode";
    r.category = "Settings";
    r.relevanceScore = 0.85;
    r.tags = {"theme", "dark"};
    EXPECT_EQ(r.id, "res_1");
    EXPECT_EQ(r.title, "Dark Mode");
    EXPECT_EQ(r.category, "Settings");
    EXPECT_EQ(r.relevanceScore, 0.85);
    EXPECT_EQ(r.tags.size(), 2u);
}

// ── Suggestion Tests ──────────────────────────────────────────────────

TEST(SuggestionTest, DefaultConstruction) {
    Suggestion s;
    EXPECT_TRUE(s.text.empty());
    EXPECT_EQ(s.weight, 0.0);
}

TEST(SuggestionTest, CustomSuggestion) {
    Suggestion s;
    s.text = "dark mode";
    s.category = "history";
    s.weight = 0.9;
    EXPECT_EQ(s.text, "dark mode");
    EXPECT_EQ(s.category, "history");
    EXPECT_EQ(s.weight, 0.9);
}

// ── CancellationToken Tests ───────────────────────────────────────────

TEST(CancellationTokenTest, InitiallyNotCancelled) {
    SimpleCancellationToken token;
    EXPECT_FALSE(token.isCancelled());
}

TEST(CancellationTokenTest, CancelSetsFlag) {
    SimpleCancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.isCancelled());
}

TEST(CancellationTokenTest, MultipleCancelsIdempotent) {
    SimpleCancellationToken token;
    token.cancel();
    token.cancel();
    EXPECT_TRUE(token.isCancelled());
}

TEST(CancellationTokenTest, PolymorphicUsage) {
    auto token = std::make_shared<SimpleCancellationToken>();
    ICancellationToken* base = token.get();
    EXPECT_FALSE(base->isCancelled());
    token->cancel();
    EXPECT_TRUE(base->isCancelled());
}

// ── ProviderRegistry Tests ────────────────────────────────────────────

class ProviderRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        providerA = std::make_unique<MockSearchProvider>("provider.a", "Provider A", 1);
        providerB = std::make_unique<MockSearchProvider>("provider.b", "Provider B", 2);
    }

    std::unique_ptr<MockSearchProvider> providerA;
    std::unique_ptr<MockSearchProvider> providerB;
    ProviderRegistry registry;
};

TEST_F(ProviderRegistryTest, InitiallyEmpty) {
    EXPECT_TRUE(registry.getAll().empty());
}

TEST_F(ProviderRegistryTest, AddProvider) {
    EXPECT_TRUE(registry.add(providerA.get()));
    EXPECT_EQ(registry.getAll().size(), 1u);
}

TEST_F(ProviderRegistryTest, AddDuplicateFails) {
    EXPECT_TRUE(registry.add(providerA.get()));
    EXPECT_FALSE(registry.add(providerA.get()));
}

TEST_F(ProviderRegistryTest, GetReturnsCorrect) {
    registry.add(providerA.get());
    auto* retrieved = registry.get("provider.a");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->getMetadata().id, "provider.a");
}

TEST_F(ProviderRegistryTest, GetNonExistentReturnsNull) {
    EXPECT_EQ(registry.get("nonexistent"), nullptr);
}

TEST_F(ProviderRegistryTest, RemoveProvider) {
    registry.add(providerA.get());
    EXPECT_TRUE(registry.remove("provider.a"));
    EXPECT_TRUE(registry.getAll().empty());
}

TEST_F(ProviderRegistryTest, RemoveNonExistentReturnsFalse) {
    EXPECT_FALSE(registry.remove("nonexistent"));
}

TEST_F(ProviderRegistryTest, GetAllReturnsAll) {
    registry.add(providerA.get());
    registry.add(providerB.get());
    auto all = registry.getAll();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(ProviderRegistryTest, AddMultipleDistinctIds) {
    EXPECT_TRUE(registry.add(providerA.get()));
    EXPECT_TRUE(registry.add(providerB.get()));
    EXPECT_EQ(registry.getAll().size(), 2u);
}

// ── SearchHistory Tests ───────────────────────────────────────────────

class SearchHistoryTest : public ::testing::Test {
protected:
    SearchHistory history;
};

TEST_F(SearchHistoryTest, InitiallyEmpty) {
    EXPECT_TRUE(history.getRecent(10).empty());
}

TEST_F(SearchHistoryTest, AddQuery) {
    history.add("dark mode");
    auto recent = history.getRecent(10);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0], "dark mode");
}

TEST_F(SearchHistoryTest, AddMultipleMostRecentFirst) {
    history.add("first");
    history.add("second");
    auto recent = history.getRecent(10);
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0], "second");
    EXPECT_EQ(recent[1], "first");
}

TEST_F(SearchHistoryTest, DeduplicationMovesToFront) {
    history.add("first");
    history.add("second");
    history.add("first");
    auto recent = history.getRecent(10);
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0], "first");
    EXPECT_EQ(recent[1], "second");
}

TEST_F(SearchHistoryTest, MaxCountRespected) {
    for (int i = 0; i < 50; ++i) {
        history.add("query_" + std::to_string(i));
    }
    auto recent = history.getRecent(5);
    EXPECT_EQ(recent.size(), 5u);
}

TEST_F(SearchHistoryTest, ClearEmptiesHistory) {
    history.add("something");
    history.clear();
    EXPECT_TRUE(history.getRecent(10).empty());
}

TEST_F(SearchHistoryTest, MaxEntriesEviction) {
    for (int i = 0; i < 150; ++i) {
        history.add("query_" + std::to_string(i));
    }
    auto recent = history.getRecent(200);
    EXPECT_LE(recent.size(), 100u);
}

TEST_F(SearchHistoryTest, ThreadSafeAdd) {
    std::thread t1([&]() { history.add("from_t1"); });
    std::thread t2([&]() { history.add("from_t2"); });
    t1.join();
    t2.join();
    auto recent = history.getRecent(10);
    EXPECT_EQ(recent.size(), 2u);
}

// ── RankingEngine Tests ───────────────────────────────────────────────

class RankingEngineTest : public ::testing::Test {
protected:
    RankingEngine engine;

    SearchResult makeResult(const std::string& title, double providerScore,
                            const std::string& category = "",
                            const std::chrono::system_clock::time_point& timestamp = {}) {
        SearchResult r;
        r.title = title;
        r.description = "desc";
        r.category = category;
        r.relevanceScore = providerScore;
        r.timestamp = timestamp;
        return r;
    }
};

TEST_F(RankingEngineTest, EmptyInput) {
    std::vector<SearchResult> results;
    SearchQuery q;
    engine.rank(results, q);
    EXPECT_TRUE(results.empty());
}

TEST_F(RankingEngineTest, SingleResultKept) {
    auto r = makeResult("test", 0.5);
    std::vector<SearchResult> results = {r};
    SearchQuery q;
    q.rawText = "test";
    engine.rank(results, q);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GT(results[0].relevanceScore, 0.5);
}

TEST_F(RankingEngineTest, HigherProviderScoreRanksFirst) {
    auto r1 = makeResult("result A", 0.9);
    auto r2 = makeResult("result B", 0.3);
    std::vector<SearchResult> results = {r2, r1};
    SearchQuery q;
    engine.rank(results, q);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_GT(results[0].relevanceScore, results[1].relevanceScore);
    EXPECT_EQ(results[0].title, "result A");
}

TEST_F(RankingEngineTest, ExactMatchBoost) {
    auto r = makeResult("exact match", 0.5);
    std::vector<SearchResult> results = {r};
    SearchQuery q;
    q.rawText = "exact match";
    engine.rank(results, q);
    EXPECT_GT(results[0].relevanceScore, 0.75);
}

TEST_F(RankingEngineTest, CategoryContextBoost) {
    auto r = makeResult("some result", 0.5, "currentModule");
    std::vector<SearchResult> results = {r};
    SearchQuery q;
    q.context.currentModule = "currentModule";
    engine.rank(results, q);
    EXPECT_GT(results[0].relevanceScore, 0.5);
}

TEST_F(RankingEngineTest, RecentResultBoost) {
    auto recent = makeResult("recent", 0.5, "", std::chrono::system_clock::now());
    auto old = makeResult("old", 0.5, "", std::chrono::system_clock::now() - std::chrono::hours(200));
    std::vector<SearchResult> results = {old, recent};
    SearchQuery q;
    engine.rank(results, q);
    EXPECT_GT(results[0].relevanceScore, results[1].relevanceScore);
}

// ── SuggestionService Tests ───────────────────────────────────────────

class SuggestionServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto* prov = new MockSearchProvider("kw.provider", "KW Provider",
                                            0, SearchCapability::None,
                                            {}, {"dark", "mode", "light"});
        keywordProvider_.reset(prov);
        registry_.add(prov);
        history_.add("dark mode");
        history_.add("light theme");
        history_.add("settings");
    }

    ProviderRegistry registry_;
    SearchHistory history_;
    std::unique_ptr<MockSearchProvider> keywordProvider_;
};

TEST_F(SuggestionServiceTest, GetSuggestionsFromHistory) {
    SuggestionService svc(history_, registry_);
    auto suggestions = svc.getSuggestions("dark", 5);
    ASSERT_FALSE(suggestions.empty());
    bool found = false;
    for (auto& s : suggestions) {
        if (s.text == "dark mode") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(SuggestionServiceTest, GetSuggestionsFromKeywords) {
    SuggestionService svc(history_, registry_);
    auto suggestions = svc.getSuggestions("lig", 5);
    bool found = false;
    for (auto& s : suggestions) {
        if (s.text == "light") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(SuggestionServiceTest, MaxResultsRespected) {
    SuggestionService svc(history_, registry_);
    auto suggestions = svc.getSuggestions("d", 2);
    EXPECT_LE(suggestions.size(), 2u);
}

TEST_F(SuggestionServiceTest, NoMatchReturnsEmpty) {
    SuggestionService svc(history_, registry_);
    auto suggestions = svc.getSuggestions("xyznonexistent", 5);
    EXPECT_TRUE(suggestions.empty());
}

// ── SearchService Tests ───────────────────────────────────────────────

class SearchServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = std::make_unique<ProviderRegistry>();
        rankingEngine_ = std::make_unique<RankingEngine>();
        queryEngine_ = std::make_unique<QueryEngine>(*registry_, *rankingEngine_, nullptr);
        history_ = std::make_unique<SearchHistory>();
        suggestionService_ = std::make_unique<SuggestionService>(*history_, *registry_);

        provider_ = std::make_unique<MockSearchProvider>("test.provider", "Test Provider", 5);

        SearchResult r;
        r.id = "res_1";
        r.title = "Dark Mode Setting";
        r.category = "Settings";
        r.relevanceScore = 0.8;
        r.timestamp = std::chrono::system_clock::now();
        provider_->addCannedResult(r);

        registry_->add(provider_.get());

        service_ = std::make_unique<SearchService>(
            std::move(registry_),
            std::move(queryEngine_),
            std::move(rankingEngine_),
            std::move(suggestionService_),
            std::move(history_),
            bus_);
    }

    EventBus bus_;
    std::unique_ptr<ProviderRegistry> registry_;
    std::unique_ptr<RankingEngine> rankingEngine_;
    std::unique_ptr<QueryEngine> queryEngine_;
    std::unique_ptr<SearchHistory> history_;
    std::unique_ptr<SuggestionService> suggestionService_;
    std::unique_ptr<MockSearchProvider> provider_;
    std::unique_ptr<SearchService> service_;
};

TEST_F(SearchServiceTest, RegisterProvider) {
    auto prov2 = std::make_unique<MockSearchProvider>("test.provider2", "Provider 2");
    EXPECT_TRUE(service_->registerProvider(prov2.get()));
    auto providers = service_->getProviders();
    EXPECT_EQ(providers.size(), 2u);
}

TEST_F(SearchServiceTest, RegisterDuplicateFails) {
    auto prov2 = std::make_unique<MockSearchProvider>("test.provider", "Duplicate");
    EXPECT_FALSE(service_->registerProvider(prov2.get()));
}

TEST_F(SearchServiceTest, UnregisterProvider) {
    EXPECT_TRUE(service_->unregisterProvider("test.provider"));
    EXPECT_TRUE(service_->getProviders().empty());
}

TEST_F(SearchServiceTest, UnregisterNonExistentReturnsFalse) {
    EXPECT_FALSE(service_->unregisterProvider("nonexistent"));
}

TEST_F(SearchServiceTest, SearchReturnsResults) {
    SearchQuery q;
    q.rawText = "dark mode";
    auto results = service_->search(q);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].title, "Dark Mode Setting");
}

TEST_F(SearchServiceTest, SearchWithCancellation) {
    auto token = std::make_shared<SimpleCancellationToken>();
    SearchQuery q;
    q.rawText = "test";
    auto results = service_->searchWithCancellation(q, token);
    EXPECT_FALSE(results.empty());
}

TEST_F(SearchServiceTest, SearchWithCancelledToken) {
    auto token = std::make_shared<SimpleCancellationToken>();
    token->cancel();
    SearchQuery q;
    q.rawText = "test";
    auto results = service_->searchWithCancellation(q, token);
    EXPECT_TRUE(results.empty());
}

TEST_F(SearchServiceTest, AddToHistory) {
    service_->addToHistory("my search");
    auto recent = service_->getRecentQueries(5);
    ASSERT_FALSE(recent.empty());
    EXPECT_EQ(recent[0], "my search");
}

TEST_F(SearchServiceTest, ClearHistory) {
    service_->addToHistory("something");
    service_->clearHistory();
    EXPECT_TRUE(service_->getRecentQueries(5).empty());
}

TEST_F(SearchServiceTest, GetProviders) {
    auto providers = service_->getProviders();
    ASSERT_EQ(providers.size(), 1u);
    EXPECT_EQ(providers[0].id, "test.provider");
}

TEST_F(SearchServiceTest, GetSuggestionsDelegates) {
    service_->addToHistory("dark mode test");
    auto suggestions = service_->getSuggestions("dark", 3);
    ASSERT_FALSE(suggestions.empty());
}

TEST_F(SearchServiceTest, QueryCountIncrements) {
    int before = service_->getQueryCount();
    SearchQuery q;
    service_->search(q);
    EXPECT_EQ(service_->getQueryCount(), before + 1);
}

// ── SearchEvents Tests ────────────────────────────────────────────────

TEST(SearchEventsTest, ConstantsDefined) {
    SearchQueryExecutedEvent eq{};
    EXPECT_TRUE(eq.queryText.empty());
    EXPECT_EQ(eq.resultCount, 0);
    EXPECT_EQ(eq.durationMs, 0);

    ProviderRegisteredEvent pr{};
    EXPECT_TRUE(pr.providerId.empty());

    ProviderUnregisteredEvent pu{};
    EXPECT_TRUE(pu.providerId.empty());

    HistoryClearedEvent hc{};
    (void)hc;
}

// ── SearchService with Event Bus Tests ────────────────────────────────

TEST(SearchServiceWithBusTest, PublishOnRegister) {
    EventBus bus;
    bool eventReceived = false;
    bus.subscribe([&](const ModuleEvent& ev) {
        if (ev.moduleId == "rex.search" &&
            ev.error.find("provider_registered") != std::string::npos) {
            eventReceived = true;
        }
    });

    ProviderRegistry reg;
    RankingEngine rank;
    QueryEngine qe(reg, rank, nullptr);
    SearchHistory hist;
    SuggestionService sugg(hist, reg);
    SearchService svc(
        std::make_unique<ProviderRegistry>(),
        std::make_unique<QueryEngine>(reg, rank, nullptr),
        std::make_unique<RankingEngine>(),
        std::make_unique<SuggestionService>(hist, reg),
        std::make_unique<SearchHistory>(),
        bus);

    MockSearchProvider prov("event.test", "Event Test");
    svc.registerProvider(&prov);
    EXPECT_TRUE(eventReceived);
}

// ──────────────────────────────────────────────────────────────────────────
// Phase 2: ProviderHealth Tests
// ──────────────────────────────────────────────────────────────────────────

TEST(ProviderHealthTest, DefaultConstruction) {
    ProviderHealth h;
    EXPECT_TRUE(h.providerId.empty());
    EXPECT_EQ(h.documentCount, 0u);
    EXPECT_EQ(h.memoryBytes, 0u);
    EXPECT_EQ(h.status, "ok");
    EXPECT_EQ(h.errorCount, 0);
}

TEST(ProviderHealthTest, CustomValues) {
    ProviderHealth h;
    h.providerId = "test.provider";
    h.documentCount = 100;
    h.memoryBytes = 4096;
    h.status = "degraded";
    h.errorCount = 3;
    h.lastError = "timeout";
    EXPECT_EQ(h.providerId, "test.provider");
    EXPECT_EQ(h.documentCount, 100u);
    EXPECT_EQ(h.memoryBytes, 4096u);
    EXPECT_EQ(h.status, "degraded");
    EXPECT_EQ(h.errorCount, 3);
    EXPECT_EQ(h.lastError, "timeout");
}

// ── SearchSession Tests ─────────────────────────────────────────────────

TEST(SearchSessionTest, DefaultConstruction) {
    SearchSession s;
    EXPECT_TRUE(s.sessionId.empty());
    EXPECT_FALSE(s.cancelled);
    EXPECT_TRUE(s.clickedResultId.empty());
}

TEST(SearchSessionTest, CustomSession) {
    SearchSession s;
    s.sessionId = "session_1";
    s.cancelled = true;
    s.clickedResultId = "res_42";
    EXPECT_EQ(s.sessionId, "session_1");
    EXPECT_TRUE(s.cancelled);
    EXPECT_EQ(s.clickedResultId, "res_42");
}

// ── MetricsCollector Tests ──────────────────────────────────────────────

class MetricsCollectorTest : public ::testing::Test {
protected:
    MetricsCollector collector;
};

TEST_F(MetricsCollectorTest, InitiallyZero) {
    auto m = collector.getMetrics();
    EXPECT_EQ(m.totalQueries, 0);
    EXPECT_EQ(m.totalTimeouts, 0);
    EXPECT_TRUE(m.providerQueries.empty());
}

TEST_F(MetricsCollectorTest, RecordQueryIncrements) {
    collector.recordQuery("prov.a", std::chrono::milliseconds(50));
    auto m = collector.getMetrics();
    EXPECT_EQ(m.totalQueries, 1);
}

TEST_F(MetricsCollectorTest, RecordQueryTracksPerProvider) {
    collector.recordQuery("prov.a", std::chrono::milliseconds(10));
    collector.recordQuery("prov.b", std::chrono::milliseconds(20));
    collector.recordQuery("prov.a", std::chrono::milliseconds(30));
    auto m = collector.getMetrics();
    EXPECT_EQ(m.totalQueries, 3);
    EXPECT_EQ(m.providerQueries.at("prov.a"), 2);
    EXPECT_EQ(m.providerQueries.at("prov.b"), 1);
}

TEST_F(MetricsCollectorTest, RecordTimeout) {
    collector.recordTimeout("prov.a");
    auto m = collector.getMetrics();
    EXPECT_EQ(m.totalTimeouts, 1);
}

TEST_F(MetricsCollectorTest, ResetClearsMetrics) {
    collector.recordQuery("prov.a", std::chrono::milliseconds(10));
    collector.reset();
    auto m = collector.getMetrics();
    EXPECT_EQ(m.totalQueries, 0);
    EXPECT_EQ(m.totalTimeouts, 0);
}

// ── IndexCoordinator Tests ──────────────────────────────────────────────

class IndexCoordinatorTest : public ::testing::Test {
protected:
    IndexCoordinator index;

    SearchResult makeDoc(const std::string& id, const std::string& title,
                         const std::string& description = "") {
        SearchResult r;
        r.id = id;
        r.title = title;
        r.description = description;
        return r;
    }
};

TEST_F(IndexCoordinatorTest, InitiallyEmptySearch) {
    auto hits = index.search({"test"});
    EXPECT_TRUE(hits.empty());
}

TEST_F(IndexCoordinatorTest, IndexAndSearch) {
    index.indexDocument("prov", makeDoc("doc1", "dark mode setting"));
    auto hits = index.search({"dark"});
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].first, "prov:doc1");
}

TEST_F(IndexCoordinatorTest, MultipleDocs) {
    index.indexDocument("prov", makeDoc("doc1", "dark mode setting"));
    index.indexDocument("prov", makeDoc("doc2", "light theme"));
    auto hits = index.search({"dark"});
    ASSERT_EQ(hits.size(), 1u);
    auto hits2 = index.search({"light"});
    ASSERT_EQ(hits2.size(), 1u);
}

TEST_F(IndexCoordinatorTest, TokenFrequencyBoostsScore) {
    index.indexDocument("prov", makeDoc("doc1", "dark dark dark mode"));
    index.indexDocument("prov", makeDoc("doc2", "dark mode"));
    auto hits = index.search({"dark"});
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_GT(hits[0].second, hits[1].second);
    EXPECT_EQ(hits[0].first, "prov:doc1");
}

TEST_F(IndexCoordinatorTest, SearchWithProviderFilter) {
    index.indexDocument("prov.a", makeDoc("doc1", "dark mode"));
    index.indexDocument("prov.b", makeDoc("doc2", "dark theme"));
    auto hits = index.search({"dark"}, "prov.a");
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].first, "prov.a:doc1");
}

TEST_F(IndexCoordinatorTest, RemoveDocument) {
    index.indexDocument("prov", makeDoc("doc1", "dark mode"));
    index.removeDocument("prov", "doc1");
    auto hits = index.search({"dark"});
    EXPECT_TRUE(hits.empty());
}

TEST_F(IndexCoordinatorTest, IndexDocumentUpdatesExisting) {
    index.indexDocument("prov", makeDoc("doc1", "dark mode"));
    index.indexDocument("prov", makeDoc("doc1", "dark mode setting"));
    auto hits = index.search({"setting"});
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].first, "prov:doc1");
}

TEST_F(IndexCoordinatorTest, ThreadSafeAdd) {
    std::thread t1([&]() { index.indexDocument("prov", makeDoc("t1", "thread one")); });
    std::thread t2([&]() { index.indexDocument("prov", makeDoc("t2", "thread two")); });
    t1.join();
    t2.join();
    auto hits = index.search({"thread"});
    EXPECT_EQ(hits.size(), 2u);
}

// ── QueryEngine Health Tests ────────────────────────────────────────────

class QueryEngineHealthTest : public ::testing::Test {
protected:
    ProviderRegistry reg;
    RankingEngine rank;
    QueryEngine engine{reg, rank, nullptr};
};

TEST_F(QueryEngineHealthTest, UnknownHealthReturnsStatusUnknown) {
    auto h = engine.getHealth("nonexistent");
    EXPECT_EQ(h.status, "unknown");
    EXPECT_EQ(h.providerId, "nonexistent");
}

TEST_F(QueryEngineHealthTest, UpdateHealth) {
    ProviderHealth h;
    h.providerId = "prov.a";
    h.documentCount = 50;
    h.status = "ok";
    engine.updateHealth("prov.a", h);
    auto retrieved = engine.getHealth("prov.a");
    EXPECT_EQ(retrieved.status, "ok");
    EXPECT_EQ(retrieved.documentCount, 50u);
}

TEST_F(QueryEngineHealthTest, GetAllHealth) {
    ProviderHealth h1;
    h1.providerId = "prov.a";
    h1.status = "ok";
    ProviderHealth h2;
    h2.providerId = "prov.b";
    h2.status = "degraded";
    engine.updateHealth("prov.a", h1);
    engine.updateHealth("prov.b", h2);
    auto all = engine.getAllHealth();
    EXPECT_EQ(all.size(), 2u);
}

// ── SearchService Phase 2: Sessions ────────────────────────────────────

class SearchServicePhase2Test : public ::testing::Test {
protected:
    void SetUp() override {
        auto reg = std::make_unique<ProviderRegistry>();
        auto rank = std::make_unique<RankingEngine>();
        auto qe = std::make_unique<QueryEngine>(*reg, *rank, nullptr);
        auto hist = std::make_unique<SearchHistory>();
        auto sugg = std::make_unique<SuggestionService>(*hist, *reg);
        provider_ = std::make_unique<MockSearchProvider>("test.prov", "Test");
        SearchResult r;
        r.id = "res_1";
        r.title = "result";
        r.relevanceScore = 0.8;
        r.timestamp = std::chrono::system_clock::now();
        provider_->addCannedResult(r);
        reg->add(provider_.get());
        service_ = std::make_unique<SearchService>(
            std::move(reg), std::move(qe), std::move(rank),
            std::move(sugg), std::move(hist), bus_);
    }

    EventBus bus_;
    std::unique_ptr<MockSearchProvider> provider_;
    std::unique_ptr<SearchService> service_;
};

TEST_F(SearchServicePhase2Test, StartSessionReturnsId) {
    SearchQuery q;
    q.rawText = "test";
    auto id = service_->startSession(q);
    EXPECT_FALSE(id.empty());
}

TEST_F(SearchServicePhase2Test, StartSessionIncrementsCounter) {
    auto id1 = service_->startSession(SearchQuery{});
    auto id2 = service_->startSession(SearchQuery{});
    EXPECT_NE(id1, id2);
}

TEST_F(SearchServicePhase2Test, RecordClick) {
    auto id = service_->startSession(SearchQuery{});
    service_->recordClick(id, "res_42");
    auto session = service_->getSession(id);
    EXPECT_EQ(session.clickedResultId, "res_42");
}

TEST_F(SearchServicePhase2Test, CancelSession) {
    auto id = service_->startSession(SearchQuery{});
    service_->cancelSession(id);
    auto session = service_->getSession(id);
    EXPECT_TRUE(session.cancelled);
}

TEST_F(SearchServicePhase2Test, GetNonExistentSessionReturnsEmpty) {
    auto s = service_->getSession("nonexistent");
    EXPECT_TRUE(s.sessionId.empty());
}

TEST_F(SearchServicePhase2Test, GetAllSessions) {
    service_->startSession(SearchQuery{});
    service_->startSession(SearchQuery{});
    auto all = service_->getAllSessions();
    EXPECT_EQ(all.size(), 2u);
}

// ── SearchService Phase 2: Metrics ─────────────────────────────────────

TEST_F(SearchServicePhase2Test, MetricsInitiallyZero) {
    auto m = service_->getMetrics();
    EXPECT_EQ(m.totalQueries, 0);
}

TEST_F(SearchServicePhase2Test, SearchRecordsMetrics) {
    SearchQuery q;
    q.rawText = "test";
    service_->search(q);
    auto m = service_->getMetrics();
    EXPECT_GT(m.totalQueries, 0);
}

TEST_F(SearchServicePhase2Test, ResetMetrics) {
    SearchQuery q;
    service_->search(q);
    service_->resetMetrics();
    auto m = service_->getMetrics();
    EXPECT_EQ(m.totalQueries, 0);
}

TEST_F(SearchServicePhase2Test, GetQueryCountDelegatesToMetrics) {
    SearchQuery q;
    q.rawText = "test";
    int before = service_->getQueryCount();
    service_->search(q);
    EXPECT_EQ(service_->getQueryCount(), before + 1);
}

// ── SearchService Phase 2: Health ──────────────────────────────────────

TEST_F(SearchServicePhase2Test, UpdateAndGetHealth) {
    ProviderHealth h;
    h.providerId = "test.prov";
    h.documentCount = 42;
    h.status = "ok";
    service_->updateHealth("test.prov", h);
    auto retrieved = service_->getHealth("test.prov");
    EXPECT_EQ(retrieved.status, "ok");
    EXPECT_EQ(retrieved.documentCount, 42u);
}

TEST_F(SearchServicePhase2Test, GetAllHealth) {
    ProviderHealth h;
    h.providerId = "prov.a";
    service_->updateHealth("prov.a", h);
    auto all = service_->getAllHealth();
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all[0].providerId, "prov.a");
}

// ──────────────────────────────────────────────────────────────────────────
// Phase 0: Semantic Search Tests
// ──────────────────────────────────────────────────────────────────────────

class MockEmbeddingService : public semantic::IEmbeddingService {
public:
    explicit MockEmbeddingService(int dim = 4) : dim_(dim) {}

    semantic::Embedding embed(const std::string& text) override {
        // Deterministic embedding based on text hash
        semantic::Embedding emb(dim_, 0.0f);
        size_t hash = std::hash<std::string>{}(text);
        for (int i = 0; i < dim_; ++i) {
            emb[i] = static_cast<float>((hash >> (i * 4)) & 0xFF) / 255.0f;
        }
        return emb;
    }

    int dimension() const override { return dim_; }

private:
    int dim_;
};

TEST(EmbeddingServiceTest, MockProducesFixedDimension) {
    MockEmbeddingService svc(8);
    EXPECT_EQ(svc.dimension(), 8);
}

TEST(EmbeddingServiceTest, MockProducesConsistentEmbeddings) {
    MockEmbeddingService svc;
    auto e1 = svc.embed("hello world");
    auto e2 = svc.embed("hello world");
    ASSERT_EQ(e1.size(), e2.size());
    for (size_t i = 0; i < e1.size(); ++i) {
        EXPECT_EQ(e1[i], e2[i]);
    }
}

TEST(EmbeddingServiceTest, DifferentTextsDifferentEmbeddings) {
    MockEmbeddingService svc;
    auto e1 = svc.embed("apple");
    auto e2 = svc.embed("banana");
    bool allEqual = true;
    for (size_t i = 0; i < e1.size(); ++i) {
        if (e1[i] != e2[i]) { allEqual = false; break; }
    }
    EXPECT_FALSE(allEqual);
}

// ── SemanticIndex Tests ────────────────────────────────────────────────

class SemanticIndexTest : public ::testing::Test {
protected:
    std::shared_ptr<MockEmbeddingService> embedder_{
        std::make_shared<MockEmbeddingService>()};
    semantic::SemanticIndex index{embedder_};

    SearchResult makeResult(const std::string& id, const std::string& title) {
        SearchResult r;
        r.id = id;
        r.title = title;
        r.providerId = "prov";
        return r;
    }
};

TEST_F(SemanticIndexTest, InitiallyEmpty) {
    auto hits = index.search("test");
    EXPECT_TRUE(hits.empty());
}

TEST_F(SemanticIndexTest, IndexAndSearch) {
    index.indexDocument("prov", "doc1", "dark mode setting");
    auto hits = index.search("dark mode");
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].documentId, "prov:doc1");
}

TEST_F(SemanticIndexTest, IndexWithResultAndSearchWithResults) {
    index.indexDocument("prov", "doc1", "dark mode", makeResult("doc1", "Dark Mode"));
    std::vector<SearchResult> results;
    auto hits = index.searchWithResults("dark mode", 10, results);
    ASSERT_FALSE(hits.empty());
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].id, "doc1");
    EXPECT_EQ(results[0].title, "Dark Mode");
}

TEST_F(SemanticIndexTest, RemoveDocument) {
    index.indexDocument("prov", "doc1", "dark mode");
    index.removeDocument("prov", "doc1");
    auto hits = index.search("dark mode");
    EXPECT_TRUE(hits.empty());
}

TEST_F(SemanticIndexTest, GetDocument) {
    index.indexDocument("prov", "doc1", "dark mode", makeResult("doc1", "Dark Mode"));
    auto doc = index.getDocument("prov:doc1");
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->title, "Dark Mode");
}

TEST_F(SemanticIndexTest, GetNonExistentDocument) {
    auto doc = index.getDocument("prov:nonexistent");
    EXPECT_FALSE(doc.has_value());
}

TEST_F(SemanticIndexTest, MultipleDocumentsDifferentScores) {
    index.indexDocument("prov", "doc1", "apple pie recipe");
    index.indexDocument("prov", "doc2", "apple orchard");
    auto hits = index.search("apple pie", 5);
    ASSERT_FALSE(hits.empty());
    // Both docs should appear (both have "apple"), first is more similar
    bool foundDoc1 = false, foundDoc2 = false;
    for (auto& hit : hits) {
        if (hit.documentId == "prov:doc1") foundDoc1 = true;
        if (hit.documentId == "prov:doc2") foundDoc2 = true;
    }
    EXPECT_TRUE(foundDoc1);
    EXPECT_TRUE(foundDoc2);
}

TEST_F(SemanticIndexTest, TopKRespected) {
    for (int i = 0; i < 10; ++i) {
        index.indexDocument("prov", "doc" + std::to_string(i), "document " + std::to_string(i));
    }
    auto hits = index.search("document", 3);
    EXPECT_LE(hits.size(), 3u);
}

// ── ISemanticSearchProvider Tests ──────────────────────────────────────

class MockSemanticProvider : public semantic::ISemanticSearchProvider {
public:
    explicit MockSemanticProvider(const std::string& id) {
        metadata_.id = id;
        metadata_.displayName = id;
    }

    SearchProviderMetadata getMetadata() const override { return metadata_; }

    std::vector<SearchResult> search(const SearchQuery& query,
                                     ICancellationToken* cancelToken = nullptr) override {
        if (cancelToken && cancelToken->isCancelled()) return {};
        return {};
    }

    std::vector<std::string> getKeywords() const override { return {}; }

    bool isAvailable() const override { return true; }

    void indexDocuments(IndexCallback cb) override {
        cb(metadata_.id, "sem_doc1", "semantic document one");
        cb(metadata_.id, "sem_doc2", "semantic document two");
    }

    SearchProviderMetadata metadata_;
};

TEST(SemanticProviderTest, IndexDocumentsCallback) {
    auto embedder = std::make_shared<MockEmbeddingService>();
    semantic::SemanticIndex sIndex(embedder);
    MockSemanticProvider provider("sem.prov");

    int docCount = 0;
    provider.indexDocuments([&](const std::string& pid, const std::string& lid, const std::string& text) {
        SearchResult r;
        r.id = lid;
        r.providerId = pid;
        r.title = text;
        sIndex.indexDocument(pid, lid, text, r);
        docCount++;
    });

    EXPECT_EQ(docCount, 2);
    auto hits = sIndex.search("semantic");
    EXPECT_EQ(hits.size(), 2u);
}

// ── SearchService Semantic Integration ─────────────────────────────────

TEST_F(SearchServicePhase2Test, SetSemanticIndex) {
    auto embedder = std::make_shared<MockEmbeddingService>();
    auto sIndex = std::make_shared<semantic::SemanticIndex>(embedder);
    service_->setSemanticIndex(sIndex);
    EXPECT_EQ(service_->getSemanticIndex(), sIndex);
}

TEST_F(SearchServicePhase2Test, SemanticProviderAutoIndexOnRegister) {
    auto embedder = std::make_shared<MockEmbeddingService>();
    auto sIndex = std::make_shared<semantic::SemanticIndex>(embedder);
    service_->setSemanticIndex(sIndex);

    auto semProvider = std::make_unique<MockSemanticProvider>("sem.prov");
    service_->registerProvider(semProvider.get());

    // Provider should have been auto-indexed
    auto hits = sIndex->search("semantic");
    EXPECT_EQ(hits.size(), 2u);
}

// ── RankingEngine with Semantic Scores ─────────────────────────────────

TEST(RankingEngineSemanticTest, SemanticScoreBoostsRanking) {
    RankingEngine engine;
    std::unordered_map<std::string, double> scores;
    scores["prov:doc_a"] = 0.9;
    scores["prov:doc_b"] = 0.1;
    engine.setSemanticScores(scores);

    SearchResult a; a.id = "doc_a"; a.providerId = "prov"; a.relevanceScore = 0.5;
    SearchResult b; b.id = "doc_b"; b.providerId = "prov"; b.relevanceScore = 0.5;
    std::vector<SearchResult> results = {b, a};
    SearchQuery q;
    engine.rank(results, q);

    // doc_a should rank higher due to semantic score boost
    EXPECT_GT(results[0].relevanceScore, results[1].relevanceScore);
    EXPECT_EQ(results[0].id, "doc_a");
}

// ── QueryEngine Semantic Integration ───────────────────────────────────

class QueryEngineSemanticTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_shared<MockEmbeddingService>();
        semanticIndex_ = std::make_shared<semantic::SemanticIndex>(embedder_);
        semanticIndex_->indexDocument("prov", "doc1", "dark mode setting");
        semanticIndex_->indexDocument("prov", "doc2", "light theme");

        qe_.setSemanticIndex(semanticIndex_);
    }

    ProviderRegistry reg_;
    RankingEngine rank_;
    QueryEngine qe_{reg_, rank_, nullptr};
    std::shared_ptr<MockEmbeddingService> embedder_;
    std::shared_ptr<semantic::SemanticIndex> semanticIndex_;
};

TEST_F(QueryEngineSemanticTest, QueryEngineHasSemanticIndex) {
    EXPECT_EQ(qe_.getSemanticIndex(), semanticIndex_);
}
