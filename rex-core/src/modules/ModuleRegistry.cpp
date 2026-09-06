#include "ModuleRegistry.hpp"
#include <algorithm>
#include <stdexcept>

namespace rex::modules {

void ModuleRegistry::registerModule(std::unique_ptr<IModule> module) {
    if (!module) return;
    const auto& id = module->getMetadata().id;
    std::lock_guard<std::mutex> lock(mutex_);
    modules_[id] = std::move(module);
    states_[id] = ModuleState::Registered;
}

std::shared_ptr<IModule> ModuleRegistry::getModule(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(id);
    return (it != modules_.end()) ? it->second : nullptr;
}

std::vector<std::shared_ptr<IModule>> ModuleRegistry::getModulesByCategory(const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<IModule>> result;
    for (const auto& [id, mod] : modules_) {
        if (mod->getMetadata().category == category)
            result.push_back(mod);
    }
    return result;
}

std::vector<std::shared_ptr<IModule>> ModuleRegistry::getAllModules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<IModule>> result;
    for (const auto& [id, mod] : modules_)
        result.push_back(mod);
    return result;
}

std::shared_ptr<IModule> ModuleRegistry::unregisterModule(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(id);
    if (it != modules_.end()) {
        states_.erase(id);
        auto ptr = it->second;
        modules_.erase(it);
        return ptr;
    }
    return nullptr;
}

ModuleState ModuleRegistry::getModuleState(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(id);
    return (it != states_.end()) ? it->second : ModuleState::Unknown;
}

void ModuleRegistry::setModuleState(const std::string& id, ModuleState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    states_[id] = state;
}

} // namespace rex::modules
