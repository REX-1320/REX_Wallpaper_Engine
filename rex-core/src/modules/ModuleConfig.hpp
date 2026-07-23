#pragma once
#include <string>
#include <vector>

namespace rex::modules {

struct ModuleConfig {
    std::string moduleId;
    bool enabled = true;
    bool autoStart = false;
    int restartCount = 0;
};

} // namespace rex::modules
