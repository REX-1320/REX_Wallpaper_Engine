#include "CommandModule.hpp"
#include "events/CommandEvents.hpp"
#include "shortcut/ShortcutManager.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "modules/EventBus.hpp"
#include "modules/ServiceContainer.hpp"

namespace rex::command {

CommandModule::CommandModule() {
    metadata_.id = "rex.command";
    metadata_.name = "Command Framework";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core", "rex.settings"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

bool CommandModule::initialize(rex::modules::IModuleContext* context) {
    auto& container = context->getServiceContainer();
    bus_ = container.getService<rex::modules::EventBus>().get();

    registry_ = std::make_unique<CommandRegistry>();
    validator_ = std::make_unique<CommandValidator>(*registry_);
    executor_ = std::make_unique<CommandExecutor>();
    history_ = std::make_unique<CommandHistory>();
    dispatcher_ = std::make_unique<CommandDispatcher>(*registry_, *validator_,
                                                      *executor_, *history_, *bus_);
    shortcutManager_ = std::make_unique<ShortcutManager>(*bus_);

    service_ = std::make_shared<CommandService>(
        std::move(registry_),
        std::move(validator_),
        std::move(executor_),
        std::move(dispatcher_),
        std::move(history_),
        std::move(shortcutManager_));

    container.registerService<ICommandService>(service_);

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool CommandModule::start() {
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void CommandModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void CommandModule::shutdown() {
    state_ = rex::modules::ModuleState::Unloaded;
}

}
