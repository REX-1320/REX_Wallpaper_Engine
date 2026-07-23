#pragma once
#include <string>
#include <vector>
#include "modules/ModuleEvents.hpp"

namespace rex::command {

struct CommandEvents {
    static constexpr const char* Registered       = "command_registered";
    static constexpr const char* Unregistered     = "command_unregistered";
    static constexpr const char* Executed         = "command_executed";
    static constexpr const char* Failed           = "command_failed";
    static constexpr const char* CannotExecute    = "command_cannot_execute";
    static constexpr const char* Undone           = "command_undone";
    static constexpr const char* Redone           = "command_redone";
    static constexpr const char* Cancelled        = "command_cancelled";
    static constexpr const char* ShortcutChanged  = "shortcut_changed";
    static constexpr const char* HistoryCleared   = "command_history_cleared";
};

}
