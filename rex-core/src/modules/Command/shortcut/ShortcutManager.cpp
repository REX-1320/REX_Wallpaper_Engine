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
    std::lock_guard lock(mutex_);
    defaultShortcuts_[commandId] = defaultShortcut;
    rebuildReverseMap();
    publishEvent(CommandEvents::ShortcutChanged, commandId);
}

void ShortcutManager::unregisterShortcut(const std::string& commandId) {
    std::lock_guard lock(mutex_);
    defaultShortcuts_.erase(commandId);
    customShortcuts_.erase(commandId);
    rebuildReverseMap();
}

void ShortcutManager::setCustomShortcut(const std::string& commandId, const Shortcut& shortcut) {
    std::lock_guard lock(mutex_);
    customShortcuts_[commandId] = shortcut;
    rebuildReverseMap();
    publishEvent(CommandEvents::ShortcutChanged, commandId);
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
    std::lock_guard lock(mutex_);
    auto it = shortcutToCommands_.find(pressedShortcut);
    if (it != shortcutToCommands_.end() && !it->second.empty()) {
        std::string cmdId = it->second.front();
        if (executionCallback_) executionCallback_(cmdId);
        return cmdId;
    }
    return "";
}

void ShortcutManager::setExecutionCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard lock(mutex_);
    executionCallback_ = std::move(callback);
}

void ShortcutManager::rebuildReverseMap() {
    shortcutToCommands_.clear();
    for (const auto& [cmdId, defaultShortcut] : defaultShortcuts_) {
        Shortcut active = customShortcuts_.count(cmdId) ? customShortcuts_[cmdId] : defaultShortcut;
        if (active.isValid()) {
            shortcutToCommands_[active].push_back(cmdId);
        }
    }
    for (const auto& [shortcut, cmds] : shortcutToCommands_) {
        if (cmds.size() > 1) {
            publishEvent("shortcut_conflict_detected", cmds.front());
        }
    }
}

}
