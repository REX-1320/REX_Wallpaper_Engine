#pragma once
#include <cstdint>

namespace rex::desktop_personalization {

struct DesktopSettings {
    int iconSize = 32;
    int iconSpacingHorizontal = 43;
    int iconSpacingVertical = 43;
    bool showLabels = true;
    bool showHiddenIcons = false;
    bool gridEnabled = true;
    bool autoRefresh = true;
};

} // namespace rex::desktop_personalization
