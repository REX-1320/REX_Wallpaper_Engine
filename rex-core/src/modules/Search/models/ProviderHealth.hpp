#pragma once
#include <cstddef>
#include <chrono>
#include <string>

namespace rex::search {

struct ProviderHealth {
    std::string providerId;
    size_t documentCount = 0;
    size_t memoryBytes = 0;
    std::chrono::system_clock::time_point lastUpdate;
    std::string status = "ok";
    int errorCount = 0;
    std::string lastError;
};
}
