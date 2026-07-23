#pragma once

namespace rex::desktop_personalization {

struct VisualEffects {
    bool transparency = true;
    bool blur = true;
    bool acrylic = true;
    bool mica = false;
    bool animations = true;
    int animationSpeedMs = 200;
    bool shadows = true;
    bool performanceMode = false;
};

} // namespace rex::desktop_personalization
