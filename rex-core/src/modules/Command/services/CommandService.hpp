#pragma once
#include "../interfaces/ICommandService.hpp"
#include "../models/CommandDefinition.hpp"
#include "../models/CommandContext.hpp"
#include "../models/CommandResult.hpp"
#include "../models/Shortcut.hpp"
#include "../models/CommandGroup.hpp"
#include "CommandRegistry.hpp"
#include "CommandDispatcher.hpp"
#include "CommandExecutor.hpp"
#include "CommandValidator.hpp"
#include "../history/CommandHistory.hpp"
#include "../shortcut/ShortcutManager.hpp"
#include <mutex>
#include <memory>
#include <unordered_map>

namespace rex::command {

class CommandService : public ICommandService {
public:
    CommandService(std::unique_ptr<CommandRegistry> registry,
                   std::unique_ptr<CommandValidator> validator,
                   std::unique_ptr<CommandExecutor> executor,
                   std::unique_ptr<CommandDispatcher> dispatcher,
                   std::unique_ptr<CommandHistory> history,
                   std::unique_ptr<ShortcutManager> shortcutManager);

    bool registerCommand(const CommandDefinition& def) override;
    bool unregisterCommand(const std::string& id) override;

    CommandResult execute(const std::string& id, const CommandContext& ctx = {}) override;
    void executeAsync(const std::string& id, const CommandContext& ctx,
                      std::function<void(CommandResult)> callback) override;
    void cancelAsync(const std::string& id) override;

    bool isEnabled(const std::string& id) const override;
    std::vector<CommandInstance> getAllCommands() const override;

    void undo() override;
    void redo() override;
    bool canUndo() const override;
    bool canRedo() const override;
    void clearHistory() override;

    void setCustomShortcut(const std::string& commandId, const Shortcut& shortcut) override;
    Shortcut getShortcut(const std::string& commandId) const override;
    std::vector<std::string> getConflictingCommands(const Shortcut& shortcut) const override;

    bool registerGroup(const CommandGroup& group);
    const CommandGroup* getGroup(const std::string& id) const;
    std::vector<CommandGroup> getAllGroups() const;

private:
    std::unique_ptr<CommandRegistry> registry_;
    std::unique_ptr<CommandValidator> validator_;
    std::unique_ptr<CommandExecutor> executor_;
    std::unique_ptr<CommandDispatcher> dispatcher_;
    std::unique_ptr<CommandHistory> history_;
    std::unique_ptr<ShortcutManager> shortcutManager_;

    std::unordered_map<std::string, CommandGroup> groups_;
    mutable std::mutex groupMutex_;
};

}
