#include "PersonalizationService.hpp"

namespace rex::desktop_personalization {

PersonalizationService::PersonalizationService(
    std::shared_ptr<platform::IPlatformAdapter> platform,
    std::shared_ptr<ThemeManager> themeMgr,
    std::shared_ptr<AppearanceManager> appearanceMgr,
    std::shared_ptr<EffectsManager> effectsMgr,
    std::shared_ptr<DesktopManager> desktopMgr,
    std::shared_ptr<ProfileManager> profileMgr,
    std::shared_ptr<SettingsManager> settingsMgr)
    : platform_(std::move(platform))
    , themeMgr_(std::move(themeMgr))
    , appearanceMgr_(std::move(appearanceMgr))
    , effectsMgr_(std::move(effectsMgr))
    , desktopMgr_(std::move(desktopMgr))
    , profileMgr_(std::move(profileMgr))
    , settingsMgr_(std::move(settingsMgr)) {}

void PersonalizationService::backupState() {
    lastGoodTheme_ = getCurrentTheme();
    lastGoodEffects_ = getCurrentEffects();
    lastGoodDesktop_ = getCurrentDesktopSettings();
}

bool PersonalizationService::validateAndApply(std::function<bool()> applyFunc) {
    backupState();

    if (!applyFunc()) {
        rollbackToLastGood();
        return false;
    }

    settingsMgr_->saveCurrentState();
    return true;
}

bool PersonalizationService::applyTheme(const Theme& theme) {
    return validateAndApply([&]() { return themeMgr_->applyTheme(theme); });
}

bool PersonalizationService::previewTheme(const Theme& theme) {
    return themeMgr_->applyTheme(theme);
}

bool PersonalizationService::applyEffects(const VisualEffects& effects) {
    return validateAndApply([&]() { return effectsMgr_->applyEffects(effects); });
}

bool PersonalizationService::applyDesktopSettings(const DesktopSettings& settings) {
    return validateAndApply([&]() { return desktopMgr_->applyDesktopSettings(settings); });
}

std::vector<Profile> PersonalizationService::listProfiles() {
    return profileMgr_->listProfiles();
}

bool PersonalizationService::loadProfile(const std::string& name) {
    Profile p = profileMgr_->loadProfile(name);
    if (!applyTheme(p.theme)) return false;
    if (!applyEffects(p.effects)) return false;
    if (!applyDesktopSettings(p.desktop)) return false;
    return true;
}

bool PersonalizationService::saveProfile(const Profile& profile) {
    return profileMgr_->saveProfile(profile);
}

bool PersonalizationService::deleteProfile(const std::string& name) {
    return profileMgr_->deleteProfile(name);
}

bool PersonalizationService::exportProfile(const std::string& name, const std::string& filePath) {
    return profileMgr_->exportProfile(name, filePath);
}

bool PersonalizationService::importProfile(const std::string& filePath) {
    return profileMgr_->importProfile(filePath);
}

bool PersonalizationService::rollbackToLastGood() {
    themeMgr_->applyTheme(lastGoodTheme_);
    effectsMgr_->applyEffects(lastGoodEffects_);
    desktopMgr_->applyDesktopSettings(lastGoodDesktop_);
    return profileMgr_->rollbackToLastGood();
}

bool PersonalizationService::resetToDefaults() {
    return settingsMgr_->resetToDefaults();
}

Theme PersonalizationService::getCurrentTheme() const {
    return themeMgr_->getCurrentTheme();
}

VisualEffects PersonalizationService::getCurrentEffects() const {
    return effectsMgr_->getCurrentEffects();
}

DesktopSettings PersonalizationService::getCurrentDesktopSettings() const {
    return desktopMgr_->getCurrentSettings();
}

} // namespace rex::desktop_personalization
