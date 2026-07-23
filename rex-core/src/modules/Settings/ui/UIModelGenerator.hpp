#pragma once
#include <vector>
#include "../registry/Setting.hpp"
#include "UIControl.hpp"

namespace rex::settings {

class UIModelGenerator {
public:
    static std::vector<ui::UIControl> generate(const std::vector<Setting>& settings);
};

}
