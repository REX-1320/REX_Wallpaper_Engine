#include "EffectsManager.hpp"

namespace rex::desktop_personalization {

EffectsManager::EffectsManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                               std::shared_ptr<rex::modules::EventBus> bus)
    : platform_(std::move(platform)), bus_(std::move(bus)) {}

bool EffectsManager::applyEffects(const VisualEffects& effects) {
    if (!platform_->setTransparency(effects.transparency)) return false;
    if (!platform_->setBlur(effects.blur)) return false;
    if (!platform_->setAcrylic(effects.acrylic)) return false;
    if (!platform_->setMica(effects.mica)) return false;
    if (!platform_->setAnimations(effects.animations)) return false;
    if (!platform_->setAnimationSpeed(effects.animationSpeedMs)) return false;

    bus_->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::EffectsChanged,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
    return true;
}

VisualEffects EffectsManager::getCurrentEffects() const {
    return {};
}

} // namespace rex::desktop_personalization
