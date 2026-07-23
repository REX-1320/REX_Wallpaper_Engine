#pragma once
#include "../models/Theme.hpp"
#include "../models/AccentColor.hpp"
#include "../platform/IPlatformAdapter.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include <memory>

namespace rex::desktop_personalization {

class ThemeManager {
public:
    ThemeManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                 std::shared_ptr<rex::modules::EventBus> bus);

    bool applyTheme(const Theme& theme);
    bool setAccentColor(const AccentColor& color);
    Theme getCurrentTheme() const;

private:
    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
