#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <mutex>
#include "interfaces/IModule.hpp"
#include "ModuleState.hpp"

namespace rex::modules {

class ModuleRegistry {
public:
    void registerModule(std::unique_ptr<IModule> module);

    IModule* getModule(const std::string& id);
    const IModule* getModule(const std::string& id) const;
    std::vector<IModule*> getModulesByCategory(const std::string& category) const;
    std::vector<IModule*> getAllModules() const;

    std::unique_ptr<IModule> unregisterModule(const std::string& id);

    ModuleState getModuleState(const std::string& id) const;
    void setModuleState(const std::string& id, ModuleState state);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<IModule>> modules_;
    std::unordered_map<std::string, ModuleState> states_;
};

} // namespace rex::modules
