#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include <memory>

namespace rex::desktop_personalization {

class IPersonalizationService;
class PersonalizationService;
class ThemeManager;
class AppearanceManager;
class EffectsManager;
class DesktopManager;
class ExplorerManager;
class TaskbarManager;
class ProfileManager;
class SettingsManager;

namespace platform {
class IPlatformAdapter;
} // namespace platform

class DesktopPersonalizationModule : public rex::modules::IModule {
public:
    DesktopPersonalizationModule();
    ~DesktopPersonalizationModule() override;

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override;
    rex::modules::ModuleState getState() const override;

    std::shared_ptr<IPersonalizationService> getService() const {
        return std::static_pointer_cast<IPersonalizationService>(service_);
    }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;
    rex::modules::IModuleContext* context_ = nullptr;

    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<PersonalizationService> service_;

    std::shared_ptr<ThemeManager> themeMgr_;
    std::shared_ptr<AppearanceManager> appearanceMgr_;
    std::shared_ptr<EffectsManager> effectsMgr_;
    std::shared_ptr<DesktopManager> desktopMgr_;
    std::shared_ptr<ExplorerManager> explorerMgr_;
    std::shared_ptr<TaskbarManager> taskbarMgr_;
    std::shared_ptr<ProfileManager> profileMgr_;
    std::shared_ptr<SettingsManager> settingsMgr_;
};

std::unique_ptr<rex::modules::IModule> createDesktopPersonalizationModule();

} // namespace rex::desktop_personalization
