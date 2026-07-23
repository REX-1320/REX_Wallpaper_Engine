#include "DesktopPersonalizationModule.hpp"
#include "services/IPersonalizationService.hpp"
#include "services/PersonalizationService.hpp"
#include "services/ThemeManager.hpp"
#include "services/AppearanceManager.hpp"
#include "services/EffectsManager.hpp"
#include "services/DesktopManager.hpp"
#include "services/ExplorerManager.hpp"
#include "services/TaskbarManager.hpp"
#include "services/ProfileManager.hpp"
#include "services/SettingsManager.hpp"
#include "platform/Win32PlatformAdapter.hpp"
#include "modules/ServiceContainer.hpp"
#include "modules/interfaces/IModuleContext.hpp"

namespace rex::desktop_personalization {

DesktopPersonalizationModule::DesktopPersonalizationModule() {
    metadata_.id = "rex.desktop_personalization";
    metadata_.name = "Desktop Personalization";
    metadata_.version = "1.0.0";
    metadata_.category = "Desktop";
    metadata_.dependencies = {"rex.core"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

DesktopPersonalizationModule::~DesktopPersonalizationModule() = default;

bool DesktopPersonalizationModule::initialize(rex::modules::IModuleContext* context) {
    context_ = context;

    auto bus = context->getService<rex::modules::EventBus>();
    auto configStore = context->getService<rex::modules::ConfigurationStore>();

    rex::modules::ServiceContainer localContainer;

    platform_ = std::make_shared<platform::Win32PlatformAdapter>();

    themeMgr_       = std::make_shared<ThemeManager>(platform_, bus);
    appearanceMgr_  = std::make_shared<AppearanceManager>(platform_, bus);
    effectsMgr_     = std::make_shared<EffectsManager>(platform_, bus);
    desktopMgr_     = std::make_shared<DesktopManager>(platform_, bus, localContainer);
    explorerMgr_    = std::make_shared<ExplorerManager>(platform_, bus);
    taskbarMgr_     = std::make_shared<TaskbarManager>(platform_, bus);
    profileMgr_     = std::make_shared<ProfileManager>(configStore, bus);
    settingsMgr_    = std::make_shared<SettingsManager>(configStore, bus);

    service_ = std::make_shared<PersonalizationService>(
        platform_, themeMgr_, appearanceMgr_, effectsMgr_, desktopMgr_,
        profileMgr_, settingsMgr_);

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool DesktopPersonalizationModule::start() {
    settingsMgr_->loadAndApply(std::static_pointer_cast<IPersonalizationService>(service_));
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void DesktopPersonalizationModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void DesktopPersonalizationModule::shutdown() {
    state_ = rex::modules::ModuleState::Unloaded;
}

const rex::modules::ModuleMetadata& DesktopPersonalizationModule::getMetadata() const {
    return metadata_;
}

rex::modules::ModuleState DesktopPersonalizationModule::getState() const {
    return state_;
}

std::unique_ptr<rex::modules::IModule> createDesktopPersonalizationModule() {
    return std::make_unique<DesktopPersonalizationModule>();
}

} // namespace rex::desktop_personalization
