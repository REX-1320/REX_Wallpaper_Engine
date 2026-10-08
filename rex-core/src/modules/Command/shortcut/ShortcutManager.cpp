#include "ShortcutManager.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include "../events/CommandEvents.hpp"
#include <algorithm>

namespace rex::command {

ShortcutManager::ShortcutManager(rex::modules::EventBus& bus) : bus_(bus) {}

void ShortcutManager::publishEvent(const std::string& eventType, const std::string& commandId) {
    rex::modules::ModuleEvent ev;
    ev.moduleId = "rex.command";
    ev.error = commandId;
    ev.type = rex::modules::ModuleEventType::SettingChanged;
    bus_.publish(ev);
}

void ShortcutManager::registerShortcut(const std::string& commandId, const Shortcut& defaultShortcut) {
    std::vector<std::pair<std::string, std::string>> conflicts;
    {
        std::lock_guard lock(mutex_);
        defaultShortcuts_[commandId] = defaultShortcut;
        conflicts = rebuildReverseMap();
    }
    publishEvent(CommandEvents::ShortcutChanged, commandId);
    for (const auto& [type, id] : conflicts) publishEvent(type, id);
}

void ShortcutManager::unregisterShortcut(const std::string& commandId) {
    std::lock_guard lock(mutex_);
    defaultShortcuts_.erase(commandId);
    customShortcuts_.erase(commandId);
    rebuildReverseMap();
}

void ShortcutManager::setCustomShortcut(const std::string& commandId, const Shortcut& shortcut) {
    std::vector<std::pair<std::string, std::string>> conflicts;
    {
        std::lock_guard lock(mutex_);
        customShortcuts_[commandId] = shortcut;
        conflicts = rebuildReverseMap();
    }
    publishEvent(CommandEvents::ShortcutChanged, commandId);
    for (const auto& [type, id] : conflicts) publishEvent(type, id);
}

Shortcut ShortcutManager::getActiveShortcut(const std::string& commandId) const {
    std::lock_guard lock(mutex_);
    auto it = customShortcuts_.find(commandId);
    if (it != customShortcuts_.end()) return it->second;
    auto it2 = defaultShortcuts_.find(commandId);
    if (it2 != defaultShortcuts_.end()) return it2->second;
    return Shortcut{};
}

std::vector<std::string> ShortcutManager::getCommandsForShortcut(const Shortcut& shortcut) const {
    std::lock_guard lock(mutex_);
    auto it = shortcutToCommands_.find(shortcut);
    if (it != shortcutToCommands_.end()) return it->second;
    return {};
}

std::string ShortcutManager::onKeyPressed(const Shortcut& pressedShortcut) {
    // Copy the decision out, then invoke the callback with the lock released:
    // the callback runs CommandService::execute, which can reach back into
    // getShortcut / setCustomShortcut — a non-recursive mutex self-deadlocks.
    std::string cmdId;
    std::function<void(const std::string&)> callback;
    {
        std::lock_guard lock(mutex_);
        auto it = shortcutToCommands_.find(pressedShortcut);
        if (it != shortcutToCommands_.end() && !it->second.empty()) {
            cmdId = it->second.front();
        }
        callback = executionCallback_;
    }
    if (!cmdId.empty() && callback) {
        callback(cmdId);
    }
    return cmdId;
}

void ShortcutManager::setExecutionCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard lock(mutex_);
    executionCallback_ = std::move(callback);
}

std::vector<std::pair<std::string, std::string>> ShortcutManager::rebuildReverseMap() {
    shortcutToCommands_.clear();
    for (const auto& [cmdId, defaultShortcut] : defaultShortcuts_) {
        Shortcut active = customShortcuts_.count(cmdId) ? customShortcuts_[cmdId] : defaultShortcut;
        if (active.isValid()) {
            shortcutToCommands_[active].push_back(cmdId);
        }
    }
    // Collect conflicts instead of publishing here: callers hold mutex_ and
    // EventBus::publish is synchronous.
    std::vector<std::pair<std::string, std::string>> conflicts;
    for (const auto& [shortcut, cmds] : shortcutToCommands_) {
        if (cmds.size() > 1) {
            conflicts.emplace_back("shortcut_conflict_detected", cmds.front());
        }
    }
    return conflicts;
}

}
