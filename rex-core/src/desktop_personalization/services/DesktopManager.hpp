#pragma once
#include "../models/DesktopSettings.hpp"
#include "../platform/IPlatformAdapter.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include "modules/ServiceContainer.hpp"
#include <memory>

namespace rex::desktop_personalization {

class DesktopManager {
public:
    DesktopManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                   std::shared_ptr<rex::modules::EventBus> bus,
                   rex::modules::ServiceContainer& container);

    bool applyDesktopSettings(const DesktopSettings& settings);
    DesktopSettings getCurrentSettings() const;

private:
    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
