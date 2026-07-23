#pragma once
#include <string>
#include <optional>
#include "../models/AccentColor.hpp"
#include "../models/VisualEffects.hpp"
#include "../models/DesktopSettings.hpp"

namespace rex::desktop_personalization::platform {

class IPlatformAdapter {
public:
    virtual ~IPlatformAdapter() = default;

    virtual bool setSystemDarkMode(bool dark) = 0;
    virtual bool setAccentColor(const AccentColor& color) = 0;
    virtual std::optional<AccentColor> getCurrentAccentColor() = 0;

    virtual bool setTransparency(bool enable) = 0;
    virtual bool setBlur(bool enable) = 0;
    virtual bool setAcrylic(bool enable) = 0;
    virtual bool setMica(bool enable) = 0;
    virtual bool setAnimations(bool enable) = 0;
    virtual bool setAnimationSpeed(int ms) = 0;

    virtual bool setDesktopIconSize(int size) = 0;
    virtual bool setDesktopIconSpacing(int horiz, int vert) = 0;
    virtual bool setDesktopLabelsVisible(bool visible) = 0;
    virtual bool refreshDesktop() = 0;

    virtual bool restartExplorer() = 0;
    virtual bool setTaskbarAutoHide(bool autoHide) = 0;
    virtual bool setTaskbarAlignment(int alignment) = 0;

    virtual bool applyAllChanges() = 0;
};

} // namespace rex::desktop_personalization::platform
