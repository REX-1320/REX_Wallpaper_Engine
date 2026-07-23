#include "ModuleState.hpp"

namespace rex::modules {

const char* toString(ModuleState state) {
    switch (state) {
        case ModuleState::Unknown:        return "Unknown";
        case ModuleState::Registered:     return "Registered";
        case ModuleState::Loading:        return "Loading";
        case ModuleState::Loaded:         return "Loaded";
        case ModuleState::Initializing:   return "Initializing";
        case ModuleState::Initialized:    return "Initialized";
        case ModuleState::Starting:       return "Starting";
        case ModuleState::Running:        return "Running";
        case ModuleState::Paused:         return "Paused";
        case ModuleState::Stopping:       return "Stopping";
        case ModuleState::Stopped:        return "Stopped";
        case ModuleState::Disabled:       return "Disabled";
        case ModuleState::Failed:         return "Failed";
        case ModuleState::Unloading:      return "Unloading";
        case ModuleState::Unloaded:       return "Unloaded";
    }
    return "Unknown";
}

} // namespace rex::modules
