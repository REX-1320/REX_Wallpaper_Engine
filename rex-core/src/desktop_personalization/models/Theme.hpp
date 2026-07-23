#pragma once
#include <string>
#include "AccentColor.hpp"

namespace rex::desktop_personalization {

struct Theme {
    std::string id;
    std::string name;
    bool darkMode = false;
    AccentColor accentColor;
    std::string wallpaperId;
    bool transparency = true;
    bool animations = true;
    std::string effectsProfile;
};

} // namespace rex::desktop_personalization
