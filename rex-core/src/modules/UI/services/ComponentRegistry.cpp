#include "ComponentRegistry.hpp"

namespace rex::ui {

void ComponentRegistry::registerComponent(const ComponentStyleDefinition& def) {
    std::lock_guard lock(mutex_);
    definitions_[def.componentType] = def;
}

const ComponentStyleDefinition* ComponentRegistry::getDefinition(const std::string& componentType) const {
    std::lock_guard lock(mutex_);
    auto it = definitions_.find(componentType);
    return it != definitions_.end() ? &it->second : nullptr;
}

}
