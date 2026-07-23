#include "CommandDispatcher.hpp"
#include "CommandRegistry.hpp"
#include "CommandValidator.hpp"
#include "CommandExecutor.hpp"
#include "../history/CommandHistory.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include "../events/CommandEvents.hpp"

namespace rex::command {

CommandDispatcher::CommandDispatcher(CommandRegistry& registry,
                                     CommandValidator& validator,
                                     CommandExecutor& executor,
                                     CommandHistory& history,
                                     rex::modules::EventBus& bus)
    : registry_(registry), validator_(validator), executor_(executor),
      history_(history), bus_(bus) {}

void CommandDispatcher::publishEvent(const std::string& eventType, const std::string& commandId) {
    rex::modules::ModuleEvent ev;
    ev.moduleId = "rex.command";
    ev.error = commandId;
    ev.type = rex::modules::ModuleEventType::SettingChanged;
    bus_.publish(ev);
}

CommandResult CommandDispatcher::dispatch(const std::string& id, const CommandContext& baseCtx) {
    const CommandDefinition* def = registry_.getDefinition(id);
    if (!def) {
        publishEvent(CommandEvents::Failed, id);
        return {false, "Command not found"};
    }

    if (!validator_.canExecute(id)) {
        publishEvent(CommandEvents::CannotExecute, id);
        return {false, "Cannot execute command"};
    }

    CommandContext ctx = baseCtx;
    ctx.bus = &bus_;

    if (def->mode != ExecutionMode::Immediate) {
        publishEvent(CommandEvents::Failed, id);
        return {false, "Async commands require dispatchAsync"};
    }

    CommandResult result = executor_.execute(*def, ctx);
    if (result.success) {
        if (def->undo) {
            history_.push(id, *def, ctx);
        }
        publishEvent(CommandEvents::Executed, id);
    } else {
        publishEvent(CommandEvents::Failed, id);
    }
    return result;
}

void CommandDispatcher::dispatchAsync(const std::string& id, const CommandContext& baseCtx,
                                      std::function<void(CommandResult)> callback) {
    const CommandDefinition* def = registry_.getDefinition(id);
    if (!def) {
        if (callback) callback({false, "Command not found"});
        return;
    }
    if (!validator_.canExecute(id)) {
        if (callback) callback({false, "Cannot execute command"});
        return;
    }
    CommandContext ctx = baseCtx;
    ctx.bus = &bus_;

    executor_.execute(*def, ctx, [this, id, def, callback](CommandResult result) {
        if (result.success) {
            publishEvent(CommandEvents::Executed, id);
        } else {
            publishEvent(CommandEvents::Failed, id);
        }
        if (callback) callback(result);
    });
}

void CommandDispatcher::cancelAsync(const std::string& id) {
    executor_.cancel(id);
    publishEvent(CommandEvents::Cancelled, id);
}

}
