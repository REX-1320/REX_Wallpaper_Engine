#pragma once
#include "CommandDefinition.hpp"
#include <chrono>

namespace rex::command {

struct CommandInstance {
    std::string id;
    const CommandDefinition* definition = nullptr;
    bool enabled = true;
    bool visible = true;
    std::chrono::steady_clock::time_point lastExecution;
};

}
