#pragma once
#include <memory>
#include "modules/ConfigurationStore.hpp"
#include "modules/EventBus.hpp"

namespace rex::desktop_personalization {

class IPersonalizationService;

class SettingsManager {
public:
    SettingsManager(std::shared_ptr<rex::modules::ConfigurationStore> configStore,
                    std::shared_ptr<rex::modules::EventBus> bus);

    bool saveCurrentState();
    bool loadAndApply(std::shared_ptr<IPersonalizationService> service);
    bool resetToDefaults();

private:
    std::shared_ptr<rex::modules::ConfigurationStore> configStore_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
