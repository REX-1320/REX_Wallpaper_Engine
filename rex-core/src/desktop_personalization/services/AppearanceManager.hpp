#pragma once
#include "../platform/IPlatformAdapter.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include <memory>

namespace rex::desktop_personalization {

class AppearanceManager {
public:
    AppearanceManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                      std::shared_ptr<rex::modules::EventBus> bus);

    bool setDesktopIconSize(int size);
    bool setDesktopIconSpacing(int horiz, int vert);
    bool setDesktopLabelsVisible(bool visible);
    bool refreshDesktop();

private:
    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
