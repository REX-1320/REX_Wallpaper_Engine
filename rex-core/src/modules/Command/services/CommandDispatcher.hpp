#pragma once
#include <string>
#include <functional>
#include <memory>
#include "../models/CommandContext.hpp"
#include "../models/CommandResult.hpp"

namespace rex::modules { class EventBus; }

namespace rex::command {

class CommandRegistry;
class CommandValidator;
class CommandExecutor;
class CommandHistory;

class CommandDispatcher {
public:
    CommandDispatcher(CommandRegistry& registry, CommandValidator& validator,
                      CommandExecutor& executor, CommandHistory& history,
                      rex::modules::EventBus& bus);

    CommandResult dispatch(const std::string& id, const CommandContext& baseCtx = {});

    void dispatchAsync(const std::string& id, const CommandContext& baseCtx,
                       std::function<void(CommandResult)> callback);

    void cancelAsync(const std::string& id);

private:
    void publishEvent(const std::string& eventType, const std::string& commandId);

    CommandRegistry& registry_;
    CommandValidator& validator_;
    CommandExecutor& executor_;
    CommandHistory& history_;
    rex::modules::EventBus& bus_;
};

}
