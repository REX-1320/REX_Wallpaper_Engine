#pragma once
#include <string>
#include <vector>
#include <functional>
#include "SettingType.hpp"
#include "SettingValue.hpp"
#include "../models/ValidationResult.hpp"

namespace rex::settings {
struct Setting {
    std::string id;
    std::string name;
    std::string description;
    std::string category;
    SettingType type;
    SettingValue defaultValue;
    SettingValue currentValue;
    bool readOnly = false;
    bool developerOnly = false;

    std::string tooltip;
    std::string icon;
    std::string group;
    int order = 0;
    bool visible = true;
    bool experimental = false;
    bool requiresRestart = false;
    bool requiresAdmin = false;
    std::vector<std::string> searchKeywords;

    std::vector<std::function<ValidationResult(const SettingValue&)>> validators;
};
}
