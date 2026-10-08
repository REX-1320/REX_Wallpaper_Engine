#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "ModuleConfig.hpp"

namespace rex::modules {

class ConfigurationStore {
public:
    explicit ConfigurationStore(const std::string& filePath);

    std::vector<ModuleConfig> loadAll();
    void saveAll(const std::vector<ModuleConfig>& configs);
    ModuleConfig loadModule(const std::string& moduleId);
    void saveModule(const ModuleConfig& config);

private:
    // Unlocked file reader: the caller must hold mutex_ (see saveModule).
    std::vector<ModuleConfig> loadAllUnlocked();
    // Writes under the caller's lock (see saveModule). Uses a temp-file +
    // atomic replace so readers never observe a half-written config.
    void saveAllUnlocked(const std::vector<ModuleConfig>& configs);

    std::string filePath_;
    mutable std::mutex mutex_;
};

} // namespace rex::modules
