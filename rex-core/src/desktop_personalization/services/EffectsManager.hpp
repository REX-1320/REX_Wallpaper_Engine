#pragma once
#include "../models/VisualEffects.hpp"
#include "../platform/IPlatformAdapter.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include <memory>

namespace rex::desktop_personalization {

class EffectsManager {
public:
    EffectsManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                   std::shared_ptr<rex::modules::EventBus> bus);

    bool applyEffects(const VisualEffects& effects);
    VisualEffects getCurrentEffects() const;

private:
    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
