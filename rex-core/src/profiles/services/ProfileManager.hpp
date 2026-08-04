#pragma once
#include "../interfaces/IProfileService.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/EventBus.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include "modules/Widgets/interfaces/IWidgetService.hpp"
#include "modules/Personalization/interfaces/IPersonalizationService.hpp"
#include "rex/wallpaper/IWallpaperEngine.hpp"
#include <mutex>
#include <thread>
#include <atomic>

namespace rex::profiles {

class ProfileManager : public IProfileService {
public:
    ProfileManager(
        std::shared_ptr<rex::modules::ConfigurationStore> configStore,
        std::shared_ptr<rex::modules::EventBus> eventBus,
        std::shared_ptr<rex::settings::ISettingsService> settingsService,
        std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
        std::shared_ptr<rex::widgets::IWidgetService> widgetService,
        std::shared_ptr<rex::personalization::IPersonalizationService> personalizationService);

    ~ProfileManager() override;

    std::vector<Profile> listProfiles() const override;
    bool createProfile(const Profile& profile) override;
    bool updateProfile(const Profile& profile) override;
    bool deleteProfile(const std::string& id) override;
    Profile getProfile(const std::string& id) const override;

    bool applyProfile(const std::string& id) override;
    bool applyProfile(const Profile& profile) override;

    void startAutoSwitch() override;
    void stopAutoSwitch() override;
    bool isAutoSwitching() const override;

    void onProfileApplied(ProfileCallback cb) override { m_appliedCallback = std::move(cb); }

private:
    std::shared_ptr<rex::modules::ConfigurationStore> m_configStore;
    std::shared_ptr<rex::modules::EventBus> m_eventBus;
    std::shared_ptr<rex::settings::ISettingsService> m_settingsService;
    std::shared_ptr<rex::wallpaper::IWallpaperEngine> m_wallpaperEngine;
    std::shared_ptr<rex::widgets::IWidgetService> m_widgetService;
    std::shared_ptr<rex::personalization::IPersonalizationService> m_personalizationService;

    ProfileCallback m_appliedCallback;
    std::atomic<bool> m_autoSwitching{false};
    std::thread m_autoSwitchThread;
    mutable std::mutex m_mutex;

    std::vector<Profile> loadAll() const;
    void saveAll(const std::vector<Profile>& profiles) const;
    void autoSwitchLoop();
    bool evaluateTrigger(const ProfileTrigger& trigger) const;
};

} // namespace rex::profiles
