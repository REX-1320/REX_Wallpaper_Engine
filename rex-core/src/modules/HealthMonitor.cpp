#include "HealthMonitor.hpp"
#include <stdexcept>

namespace rex::modules {

void HealthMonitor::reportHealth(const std::string& moduleId, const ModuleHealth& health) {
    std::lock_guard<std::mutex> lock(mutex_);
    healthMap_[moduleId] = health;
}

ModuleHealth HealthMonitor::getHealth(const std::string& moduleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = healthMap_.find(moduleId);
    if (it != healthMap_.end()) return it->second;
    return ModuleHealth{};
}

std::unordered_map<std::string, ModuleHealth> HealthMonitor::getAllHealth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return healthMap_;
}

void HealthMonitor::resetCounters(const std::string& moduleId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = healthMap_.find(moduleId);
    if (it != healthMap_.end()) {
        it->second.crashCount = 0;
        it->second.restartCount = 0;
    }
}

} // namespace rex::modules
