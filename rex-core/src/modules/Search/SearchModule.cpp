#include "SearchModule.hpp"
#include "modules/ServiceContainer.hpp"
#include "modules/EventBus.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "index/IndexCoordinator.hpp"
#include "semantic/SemanticIndex.hpp"

namespace rex::search {

SearchModule::SearchModule() {
    metadata_.id = "rex.search";
    metadata_.name = "Search Framework";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core", "rex.settings"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

bool SearchModule::initialize(rex::modules::IModuleContext* context) {
    auto& container = context->getServiceContainer();
    bus_ = container.getService<rex::modules::EventBus>().get();

    registry_ = std::make_unique<ProviderRegistry>();
    rankingEngine_ = std::make_unique<RankingEngine>();
    indexCoordinator_ = std::make_unique<IndexCoordinator>();
    queryEngine_ = std::make_unique<QueryEngine>(*registry_, *rankingEngine_, indexCoordinator_.get());
    history_ = std::make_unique<SearchHistory>();
    suggestionService_ = std::make_unique<SuggestionService>(*history_, *registry_);

    // Semantic search (optional layer)
    embedder_ = nullptr;  // no real embedding model; providers supply their own
    semanticIndex_ = nullptr;

    service_ = std::make_shared<SearchService>(
        std::move(registry_),
        std::move(queryEngine_),
        std::move(rankingEngine_),
        std::move(suggestionService_),
        std::move(history_),
        *bus_);

    container.registerService<ISearchService>(service_);
    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool SearchModule::start() {
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void SearchModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void SearchModule::shutdown() {
    state_ = rex::modules::ModuleState::Unloaded;
}

}
