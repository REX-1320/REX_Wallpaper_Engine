#pragma once
#include <string>

namespace rex::ui {

struct Theme {
    std::string id;
    std::string name;
    std::string parentThemeId;
    bool dark = false;
};

}
