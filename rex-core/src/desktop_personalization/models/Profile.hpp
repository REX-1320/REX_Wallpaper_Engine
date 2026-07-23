#pragma once
#include <string>
#include <chrono>
#include "Theme.hpp"
#include "VisualEffects.hpp"
#include "DesktopSettings.hpp"

namespace rex::desktop_personalization {

struct Profile {
    std::string name;
    std::string description;
    Theme theme;
    VisualEffects effects;
    DesktopSettings desktop;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point modified;
    int version = 1;
};

} // namespace rex::desktop_personalization
