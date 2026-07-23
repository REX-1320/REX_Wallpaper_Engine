#pragma once
#include <string>
#include <functional>
#include "CommandContext.hpp"
#include "CommandResult.hpp"
#include "ExecutionMode.hpp"
#include "Shortcut.hpp"

namespace rex::command {

struct CommandDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string category;
    std::string icon;
    Shortcut defaultShortcut;

    ExecutionMode mode = ExecutionMode::Immediate;
    bool supportsCancellation = false;

    std::function<bool()> canExecute;
    std::function<CommandResult(CommandContext&)> execute;
    std::function<void(CommandContext&)> undo;
};

}
