#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include "modules/ModuleState.hpp"
#include "services/CommandService.hpp"
#include <memory>

namespace rex::modules { class IModuleContext; class EventBus; }

namespace rex::command {

class CommandModule : public rex::modules::IModule {
public:
    CommandModule();

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    std::unique_ptr<CommandRegistry> registry_;
    std::unique_ptr<CommandValidator> validator_;
    std::unique_ptr<CommandExecutor> executor_;
    std::unique_ptr<CommandDispatcher> dispatcher_;
    std::unique_ptr<CommandHistory> history_;
    std::unique_ptr<ShortcutManager> shortcutManager_;
    std::shared_ptr<CommandService> service_;

    rex::modules::EventBus* bus_ = nullptr;
};

}
