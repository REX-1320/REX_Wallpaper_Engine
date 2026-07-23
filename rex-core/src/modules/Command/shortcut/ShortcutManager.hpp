#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "../models/Shortcut.hpp"

namespace rex::modules { class EventBus; }

namespace rex::command {

class ShortcutManager {
public:
    explicit ShortcutManager(rex::modules::EventBus& bus);

    void registerShortcut(const std::string& commandId, const Shortcut& defaultShortcut);
    void unregisterShortcut(const std::string& commandId);

    void setCustomShortcut(const std::string& commandId, const Shortcut& shortcut);
    Shortcut getActiveShortcut(const std::string& commandId) const;

    std::vector<std::string> getCommandsForShortcut(const Shortcut& shortcut) const;

    std::string onKeyPressed(const Shortcut& pressedShortcut);

    void setExecutionCallback(std::function<void(const std::string&)> callback);

private:
    void publishEvent(const std::string& eventType, const std::string& commandId);
    void rebuildReverseMap();

    struct ShortcutHash {
        std::size_t operator()(const Shortcut& s) const {
            return static_cast<std::size_t>(s.modifiers) ^ (static_cast<std::size_t>(s.keyCode) << 16);
        }
    };

    rex::modules::EventBus& bus_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Shortcut> customShortcuts_;
    std::unordered_map<std::string, Shortcut> defaultShortcuts_;
    std::unordered_map<Shortcut, std::vector<std::string>, ShortcutHash> shortcutToCommands_;
    std::function<void(const std::string&)> executionCallback_;
};

}
