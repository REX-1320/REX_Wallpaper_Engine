#pragma once
#include <string>
#include <vector>
#include "../registry/SettingValue.hpp"

namespace rex::settings::ui {

enum class ControlType {
    Toggle,
    Slider,
    Dropdown,
    ColorPicker,
    FilePicker,
    Text,
    Hotkey,
    Unknown
};

struct UIControl {
    std::string settingId;
    std::string label;
    std::string description;
    ControlType controlType;
    SettingValue currentValue;
    std::vector<std::string> options;
    int minVal = 0, maxVal = 100;
    double minDbl = 0.0, maxDbl = 1.0;
    bool readOnly = false;
    bool requiresRestart = false;
    bool requiresAdmin = false;
    std::string tooltip;
    std::string icon;
    std::string group;
    int order = 0;
};

}
