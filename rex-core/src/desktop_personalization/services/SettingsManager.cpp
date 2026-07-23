#include "SettingsManager.hpp"
#include "IPersonalizationService.hpp"

namespace rex::desktop_personalization {

SettingsManager::SettingsManager(std::shared_ptr<rex::modules::ConfigurationStore> configStore,
                                 std::shared_ptr<rex::modules::EventBus> bus)
    : configStore_(std::move(configStore)), bus_(std::move(bus)) {}

bool SettingsManager::saveCurrentState() {
    rex::modules::ModuleConfig cfg;
    cfg.moduleId = "rex.desktop_personalization";
    cfg.enabled = true;
    cfg.autoStart = true;
    configStore_->saveModule(cfg);
    return true;
}

bool SettingsManager::loadAndApply(std::shared_ptr<IPersonalizationService> service) {
    auto cfg = configStore_->loadModule("rex.desktop_personalization");
    if (!cfg.enabled) return false;
    return true;
}

bool SettingsManager::resetToDefaults() {
    rex::modules::ModuleConfig cfg;
    cfg.moduleId = "rex.desktop_personalization";
    cfg.enabled = true;
    cfg.autoStart = true;
    cfg.restartCount = 0;
    configStore_->saveModule(cfg);
    return true;
}

} // namespace rex::desktop_personalization
