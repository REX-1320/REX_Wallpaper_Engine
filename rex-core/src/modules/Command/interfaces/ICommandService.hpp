#pragma once
#include <string>
#include <vector>
#include <functional>
#include "../models/CommandDefinition.hpp"
#include "../models/CommandInstance.hpp"
#include "../models/CommandContext.hpp"
#include "../models/CommandResult.hpp"
#include "../models/Shortcut.hpp"

namespace rex::command {

class ICommandService {
public:
    virtual ~ICommandService() = default;

    virtual bool registerCommand(const CommandDefinition& def) = 0;
    virtual bool unregisterCommand(const std::string& id) = 0;

    virtual CommandResult execute(const std::string& id, const CommandContext& ctx = {}) = 0;
    virtual void executeAsync(const std::string& id, const CommandContext& ctx,
                              std::function<void(CommandResult)> callback) = 0;
    virtual void cancelAsync(const std::string& id) = 0;

    virtual bool isEnabled(const std::string& id) const = 0;
    virtual std::vector<CommandInstance> getAllCommands() const = 0;

    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual bool canUndo() const = 0;
    virtual bool canRedo() const = 0;
    virtual void clearHistory() = 0;

    virtual void setCustomShortcut(const std::string& commandId, const Shortcut& shortcut) = 0;
    virtual Shortcut getShortcut(const std::string& commandId) const = 0;
    virtual std::vector<std::string> getConflictingCommands(const Shortcut& shortcut) const = 0;
};

}
