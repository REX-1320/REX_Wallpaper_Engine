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
    std::string filePath_;
    mutable std::mutex mutex_;
};

} // namespace rex::modules
