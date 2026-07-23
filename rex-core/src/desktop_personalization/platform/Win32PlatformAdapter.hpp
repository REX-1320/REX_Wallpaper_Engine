#pragma once
#include "IPlatformAdapter.hpp"

namespace rex::desktop_personalization::platform {

class Win32PlatformAdapter : public IPlatformAdapter {
public:
    bool setSystemDarkMode(bool dark) override;
    bool setAccentColor(const AccentColor& color) override;
    std::optional<AccentColor> getCurrentAccentColor() override;

    bool setTransparency(bool enable) override;
    bool setBlur(bool enable) override;
    bool setAcrylic(bool enable) override;
    bool setMica(bool enable) override;
    bool setAnimations(bool enable) override;
    bool setAnimationSpeed(int ms) override;

    bool setDesktopIconSize(int size) override;
    bool setDesktopIconSpacing(int horiz, int vert) override;
    bool setDesktopLabelsVisible(bool visible) override;
    bool refreshDesktop() override;

    bool restartExplorer() override;
    bool setTaskbarAutoHide(bool autoHide) override;
    bool setTaskbarAlignment(int alignment) override;

    bool applyAllChanges() override;
};

} // namespace rex::desktop_personalization::platform
