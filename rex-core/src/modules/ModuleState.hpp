#pragma once

namespace rex::modules {

enum class ModuleState : int {
    Unknown,
    Registered,
    Loading,
    Loaded,
    Initializing,
    Initialized,
    Starting,
    Running,
    Paused,
    Stopping,
    Stopped,
    Disabled,
    Failed,
    Unloading,
    Unloaded
};

const char* toString(ModuleState state);

} // namespace rex::modules
