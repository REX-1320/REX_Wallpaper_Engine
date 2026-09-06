#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <mutex>
#include "interfaces/IModule.hpp"
#include "ModuleState.hpp"

namespace rex::modules {

// Ownership: the registry holds one shared reference per module; getters
// hand out shared_ptr so callers keep the object alive across
// unregisterModule() (previously raw pointers dangled when another thread
// unregistered mid-use). The DLL/code lifetime for dynamic modules is still
// owned by ModuleLoader — see unloadDynamic contract.
class ModuleRegistry {
public:
    void registerModule(std::unique_ptr<IModule> module);

    std::shared_ptr<IModule> getModule(const std::string& id) const;
    std::vector<std::shared_ptr<IModule>> getModulesByCategory(const std::string& category) const;
    std::vector<std::shared_ptr<IModule>> getAllModules() const;

    std::shared_ptr<IModule> unregisterModule(const std::string& id);

    ModuleState getModuleState(const std::string& id) const;
    void setModuleState(const std::string& id, ModuleState state);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<IModule>> modules_;
    std::unordered_map<std::string, ModuleState> states_;
};

} // namespace rex::modules
