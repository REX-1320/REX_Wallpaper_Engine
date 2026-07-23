#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::ui {

struct ComponentStyleDefinition {
    std::string componentType;

    std::unordered_map<std::string, std::string> defaultProperties;

    struct Variant {
        std::string name;
        std::unordered_map<std::string, std::string> properties;
    };
    std::vector<Variant> variants;
};

}
