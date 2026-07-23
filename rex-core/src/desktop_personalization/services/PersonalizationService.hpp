#pragma once
#include "IPersonalizationService.hpp"
#include <memory>
#include <functional>
#include "ThemeManager.hpp"
#include "AppearanceManager.hpp"
#include "EffectsManager.hpp"
#include "DesktopManager.hpp"
#include "ProfileManager.hpp"
#include "SettingsManager.hpp"
#include "../platform/IPlatformAdapter.hpp"

namespace rex::desktop_personalization {

class PersonalizationService : public IPersonalizationService {
public:
    PersonalizationService(std::shared_ptr<platform::IPlatformAdapter> platform,
                           std::shared_ptr<ThemeManager> themeMgr,
                           std::shared_ptr<AppearanceManager> appearanceMgr,
                           std::shared_ptr<EffectsManager> effectsMgr,
                           std::shared_ptr<DesktopManager> desktopMgr,
                           std::shared_ptr<ProfileManager> profileMgr,
                           std::shared_ptr<SettingsManager> settingsMgr);

    bool applyTheme(const Theme& theme) override;
    bool previewTheme(const Theme& theme) override;
    bool applyEffects(const VisualEffects& effects) override;
    bool applyDesktopSettings(const DesktopSettings& settings) override;

    std::vector<Profile> listProfiles() override;
    bool loadProfile(const std::string& name) override;
    bool saveProfile(const Profile& profile) override;
    bool deleteProfile(const std::string& name) override;
    bool exportProfile(const std::string& name, const std::string& filePath) override;
    bool importProfile(const std::string& filePath) override;

    bool rollbackToLastGood() override;
    bool resetToDefaults() override;

    Theme getCurrentTheme() const override;
    VisualEffects getCurrentEffects() const override;
    DesktopSettings getCurrentDesktopSettings() const override;

private:
    bool validateAndApply(std::function<bool()> applyFunc);
    void backupState();

    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<ThemeManager> themeMgr_;
    std::shared_ptr<AppearanceManager> appearanceMgr_;
    std::shared_ptr<EffectsManager> effectsMgr_;
    std::shared_ptr<DesktopManager> desktopMgr_;
    std::shared_ptr<ProfileManager> profileMgr_;
    std::shared_ptr<SettingsManager> settingsMgr_;

    Theme lastGoodTheme_;
    VisualEffects lastGoodEffects_;
    DesktopSettings lastGoodDesktop_;
};

} // namespace rex::desktop_personalization
