#pragma once
#include <string>
#include <functional>

namespace rex::personalization {

struct TaskbarSettings {
    int position = 0;
    bool autoHide = false;
    bool transparent = true;
    int iconSize = 0;
    bool showLabels = true;
    int cornerRadius = 8;
};

struct ExplorerSettings {
    bool acrylic = true;
    bool mica = false;
    bool compactMode = false;
    bool showNavigationPane = true;
    bool showContextMenuIcons = true;
};

struct StartMenuSettings {
    bool replaceStart = false;
    int style = 0;
    bool transparent = true;
    bool showRecommendations = true;
};

class IPersonalizationService {
public:
    virtual ~IPersonalizationService() = default;

    virtual TaskbarSettings getTaskbarSettings() const = 0;
    virtual void setTaskbarSettings(const TaskbarSettings& s) = 0;

    virtual ExplorerSettings getExplorerSettings() const = 0;
    virtual void setExplorerSettings(const ExplorerSettings& s) = 0;

    virtual StartMenuSettings getStartMenuSettings() const = 0;
    virtual void setStartMenuSettings(const StartMenuSettings& s) = 0;

    virtual void applyAll() = 0;
    virtual void restoreDefaults() = 0;
};

} // namespace
