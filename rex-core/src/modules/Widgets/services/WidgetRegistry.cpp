#include "WidgetRegistry.hpp"

namespace rex::widgets {

void WidgetRegistry::registerDefinition(const WidgetDefinition& def) {
    m_defs[def.id] = def;
}

const WidgetDefinition* WidgetRegistry::get(const std::string& id) const {
    auto it = m_defs.find(id);
    return (it != m_defs.end()) ? &it->second : nullptr;
}

std::vector<WidgetDefinition> WidgetRegistry::getAll() const {
    std::vector<WidgetDefinition> res;
    for (const auto& [id, def] : m_defs) {
        res.push_back(def);
    }
    return res;
}

} // namespace rex::widgets
