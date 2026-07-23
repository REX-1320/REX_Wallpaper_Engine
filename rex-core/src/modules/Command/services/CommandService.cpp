#include "CommandService.hpp"
#include "../interfaces/ICommandProvider.hpp"
#include "modules/ServiceContainer.hpp"

namespace rex::command {

CommandService::CommandService(std::unique_ptr<CommandRegistry> registry,
                               std::unique_ptr<CommandValidator> validator,
                               std::unique_ptr<CommandExecutor> executor,
                               std::unique_ptr<CommandDispatcher> dispatcher,
                               std::unique_ptr<CommandHistory> history,
                               std::unique_ptr<ShortcutManager> shortcutManager)
    : registry_(std::move(registry))
    , validator_(std::move(validator))
    , executor_(std::move(executor))
    , dispatcher_(std::move(dispatcher))
    , history_(std::move(history))
    , shortcutManager_(std::move(shortcutManager))
{
    if (shortcutManager_) {
        shortcutManager_->setExecutionCallback([this](const std::string& cmdId) {
            this->execute(cmdId);
        });
    }
}

bool CommandService::registerCommand(const CommandDefinition& def) {
    bool ok = registry_->addDefinition(def);
    if (ok && shortcutManager_) {
        shortcutManager_->registerShortcut(def.id, def.defaultShortcut);
    }
    return ok;
}

bool CommandService::unregisterCommand(const std::string& id) {
    bool ok = registry_->removeDefinition(id);
    if (ok && shortcutManager_) {
        shortcutManager_->unregisterShortcut(id);
    }
    return ok;
}

CommandResult CommandService::execute(const std::string& id, const CommandContext& ctx) {
    return dispatcher_->dispatch(id, ctx);
}

void CommandService::executeAsync(const std::string& id, const CommandContext& ctx,
                                  std::function<void(CommandResult)> callback) {
    dispatcher_->dispatchAsync(id, ctx, std::move(callback));
}

void CommandService::cancelAsync(const std::string& id) {
    dispatcher_->cancelAsync(id);
}

bool CommandService::isEnabled(const std::string& id) const {
    const auto* inst = registry_->getInstance(id);
    return inst && inst->enabled && validator_->canExecute(id);
}

std::vector<CommandInstance> CommandService::getAllCommands() const {
    return registry_->getAllInstances();
}

void CommandService::undo() { history_->undo(); }

void CommandService::redo() {
    auto entry = history_->popRedoEntry();
    if (!entry) return;

    CommandContext ctx = entry->contextSnapshot;
    ctx.cancelToken = nullptr;
    ctx.service = this;
    ctx.bus = nullptr;

    execute(entry->commandId, ctx);
}

bool CommandService::canUndo() const { return history_->canUndo(); }
bool CommandService::canRedo() const { return history_->canRedo(); }
void CommandService::clearHistory() { history_->clear(); }

void CommandService::setCustomShortcut(const std::string& commandId, const Shortcut& shortcut) {
    if (shortcutManager_) {
        shortcutManager_->setCustomShortcut(commandId, shortcut);
    }
}

Shortcut CommandService::getShortcut(const std::string& commandId) const {
    if (shortcutManager_) {
        return shortcutManager_->getActiveShortcut(commandId);
    }
    const CommandDefinition* def = registry_->getDefinition(commandId);
    return def ? def->defaultShortcut : Shortcut{};
}

std::vector<std::string> CommandService::getConflictingCommands(const Shortcut& shortcut) const {
    if (shortcutManager_) {
        return shortcutManager_->getCommandsForShortcut(shortcut);
    }
    return {};
}

bool CommandService::registerGroup(const CommandGroup& group) {
    std::lock_guard lock(groupMutex_);
    if (groups_.count(group.id)) return false;
    groups_[group.id] = group;
    return true;
}

const CommandGroup* CommandService::getGroup(const std::string& id) const {
    std::lock_guard lock(groupMutex_);
    auto it = groups_.find(id);
    return it != groups_.end() ? &it->second : nullptr;
}

std::vector<CommandGroup> CommandService::getAllGroups() const {
    std::lock_guard lock(groupMutex_);
    std::vector<CommandGroup> result;
    result.reserve(groups_.size());
    for (const auto& [id, g] : groups_) result.push_back(g);
    return result;
}

}
