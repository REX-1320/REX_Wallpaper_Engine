#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include "../models/WidgetDefinition.hpp"

namespace rex::widgets {

class WidgetRegistry {
public:
    void registerDefinition(const WidgetDefinition& def);
    const WidgetDefinition* get(const std::string& id) const;
    std::vector<WidgetDefinition> getAll() const;

private:
    std::unordered_map<std::string, WidgetDefinition> m_defs;
};

} // namespace rex::widgets
