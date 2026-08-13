#pragma once
#include <string>
#include <vector>

namespace rex::modules {

struct ModuleConfig {
    std::string moduleId;
    bool enabled = true;
    bool autoStart = false;
    int restartCount = 0;
    // Settings persistence payload: a JSON object of setting id -> value
    // (stored escaped inside the config array). Empty when the module has
    // no persisted settings.
    std::string settingsJson;
};

} // namespace rex::modules
