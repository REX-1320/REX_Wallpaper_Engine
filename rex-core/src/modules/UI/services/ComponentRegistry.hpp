#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../models/ComponentStyle.hpp"

namespace rex::ui {

class ComponentRegistry {
public:
    void registerComponent(const ComponentStyleDefinition& def);

    const ComponentStyleDefinition* getDefinition(const std::string& componentType) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ComponentStyleDefinition> definitions_;
};

}
