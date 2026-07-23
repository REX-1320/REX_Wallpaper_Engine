#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include "ModuleState.hpp"

namespace rex::modules {

struct ModuleHealth {
    float cpuUsagePercent = -1.0f;
    size_t memoryBytes   = 0;
    uint32_t threadCount = 0;
    std::chrono::seconds uptime{0};
    int crashCount       = 0;
    int restartCount     = 0;
    std::string lastError;
    std::chrono::milliseconds loadTime{0};
    ModuleState state = ModuleState::Unknown;
};

} // namespace rex::modules
