#include "CommandRegistry.hpp"

namespace rex::command {

void CommandRegistry::syncInstance(const std::string& id) {
    auto itDef = definitions_.find(id);
    if (itDef != definitions_.end()) {
        auto& inst = instances_[id];
        inst.id = id;
        inst.definition = &itDef->second;
    }
}

bool CommandRegistry::addDefinition(const CommandDefinition& def) {
    std::lock_guard lock(mutex_);
    if (definitions_.count(def.id)) return false;
    definitions_[def.id] = def;
    syncInstance(def.id);
    return true;
}

bool CommandRegistry::removeDefinition(const std::string& id) {
    std::lock_guard lock(mutex_);
    bool existed = definitions_.count(id) > 0;
    definitions_.erase(id);
    instances_.erase(id);
    return existed;
}

const CommandDefinition* CommandRegistry::getDefinition(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = definitions_.find(id);
    return it != definitions_.end() ? &it->second : nullptr;
}

std::vector<CommandInstance> CommandRegistry::getAllInstances() const {
    std::lock_guard lock(mutex_);
    std::vector<CommandInstance> result;
    result.reserve(instances_.size());
    for (const auto& [id, inst] : instances_) result.push_back(inst);
    return result;
}

bool CommandRegistry::setInstanceEnabled(const std::string& id, bool enabled) {
    std::lock_guard lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    it->second.enabled = enabled;
    return true;
}

bool CommandRegistry::setInstanceVisible(const std::string& id, bool visible) {
    std::lock_guard lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    it->second.visible = visible;
    return true;
}

const CommandInstance* CommandRegistry::getInstance(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = instances_.find(id);
    return it != instances_.end() ? &it->second : nullptr;
}

}
