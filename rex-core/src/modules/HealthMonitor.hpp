#pragma once
#include <unordered_map>
#include <string>
#include <chrono>
#include <mutex>
#include "ModuleHealth.hpp"
#include "ModuleState.hpp"

namespace rex::modules {

class IModule;

class HealthMonitor {
public:
    void reportHealth(const std::string& moduleId, const ModuleHealth& health);
    ModuleHealth getHealth(const std::string& moduleId) const;
    std::unordered_map<std::string, ModuleHealth> getAllHealth() const;
    void resetCounters(const std::string& moduleId);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModuleHealth> healthMap_;
};

} // namespace rex::modules
