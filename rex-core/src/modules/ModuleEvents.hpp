#pragma once

#include <string>
#include <functional>
#include <memory>
#include "ModuleState.hpp"

namespace rex::modules {

enum class ModuleEventType {
    ModuleRegistered,
    ModuleLoaded,
    ModuleInitialized,
    ModuleStarted,
    ModuleStopped,
    ModulePaused,
    ModuleResumed,
    ModuleRestarted,
    ModuleFailed,
    ModuleDisabled,
    ModuleEnabled,
    ModuleUnloaded,

    ThemeChanged,
    EffectsChanged,
    DesktopSettingsChanged,
    ProfileSaved,
    ProfileDeleted,
    ProfileLoaded,
    RollbackApplied,

    SettingChanged,

    NotificationCreated,
    NotificationShown,
    NotificationUpdated,
    NotificationDismissed,
    NotificationActionInvoked,
    HistoryCleared,

    SearchQueryExecuted,
    SearchHistoryCleared
};

struct ModuleEvent {
    ModuleEventType type;
    std::string moduleId;
    ModuleState previousState = ModuleState::Unknown;
    ModuleState newState      = ModuleState::Unknown;
    std::string error;
};

} // namespace rex::modules
