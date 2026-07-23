#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include "modules/ModuleState.hpp"
#include "services/SearchService.hpp"
#include "semantic/EmbeddingService.hpp"
#include <memory>

namespace rex::modules { class IModuleContext; class EventBus; }

namespace rex::search {

class SearchModule : public rex::modules::IModule {
public:
    SearchModule();

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    std::unique_ptr<ProviderRegistry> registry_;
    std::unique_ptr<RankingEngine> rankingEngine_;
    std::unique_ptr<QueryEngine> queryEngine_;
    std::unique_ptr<SearchHistory> history_;
    std::unique_ptr<SuggestionService> suggestionService_;
    std::unique_ptr<IndexCoordinator> indexCoordinator_;
    std::shared_ptr<semantic::IEmbeddingService> embedder_;
    std::shared_ptr<semantic::SemanticIndex> semanticIndex_;
    std::shared_ptr<SearchService> service_;

    rex::modules::EventBus* bus_ = nullptr;
};

}
