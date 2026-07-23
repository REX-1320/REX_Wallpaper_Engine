#include "UIModelGenerator.hpp"
#include <algorithm>

namespace rex::settings {

static ui::ControlType mapType(SettingType type) {
    switch (type) {
        case SettingType::Boolean:    return ui::ControlType::Toggle;
        case SettingType::Integer:    return ui::ControlType::Slider;
        case SettingType::Float:      return ui::ControlType::Slider;
        case SettingType::Enum:       return ui::ControlType::Dropdown;
        case SettingType::Color:      return ui::ControlType::ColorPicker;
        case SettingType::Path:
        case SettingType::File:
        case SettingType::Directory:  return ui::ControlType::FilePicker;
        case SettingType::Hotkey:     return ui::ControlType::Hotkey;
        default:                      return ui::ControlType::Text;
    }
}

std::vector<ui::UIControl> UIModelGenerator::generate(const std::vector<Setting>& settings) {
    std::vector<ui::UIControl> controls;
    controls.reserve(settings.size());

    for (const auto& s : settings) {
        if (!s.visible) continue;

        ui::UIControl c;
        c.settingId        = s.id;
        c.label            = s.name;
        c.description      = s.description;
        c.controlType      = mapType(s.type);
        c.currentValue     = s.currentValue;
        c.readOnly         = s.readOnly;
        c.requiresRestart  = s.requiresRestart;
        c.requiresAdmin    = s.requiresAdmin;
        c.tooltip          = s.tooltip;
        c.icon             = s.icon;
        c.group            = s.group;
        c.order            = s.order;

        if (s.type == SettingType::Integer || s.type == SettingType::Float) {
            c.minVal = 0; c.maxVal = 100;
            c.minDbl = 0.0; c.maxDbl = 1.0;
        }
        if (s.type == SettingType::Enum) {
            c.options = {};
        }

        controls.push_back(c);
    }

    std::sort(controls.begin(), controls.end(),
              [](const auto& a, const auto& b) { return a.order < b.order; });

    return controls;
}

}
