#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../models/Theme.hpp"
#include "../models/VisualEffects.hpp"
#include "../models/DesktopSettings.hpp"
#include "../models/Profile.hpp"

namespace rex::desktop_personalization {

class IPersonalizationService {
public:
    virtual ~IPersonalizationService() = default;

    virtual bool applyTheme(const Theme& theme) = 0;
    virtual bool previewTheme(const Theme& theme) = 0;
    virtual bool applyEffects(const VisualEffects& effects) = 0;
    virtual bool applyDesktopSettings(const DesktopSettings& settings) = 0;

    virtual std::vector<Profile> listProfiles() = 0;
    virtual bool loadProfile(const std::string& name) = 0;
    virtual bool saveProfile(const Profile& profile) = 0;
    virtual bool deleteProfile(const std::string& name) = 0;
    virtual bool exportProfile(const std::string& name, const std::string& filePath) = 0;
    virtual bool importProfile(const std::string& filePath) = 0;

    virtual bool rollbackToLastGood() = 0;
    virtual bool resetToDefaults() = 0;

    virtual Theme getCurrentTheme() const = 0;
    virtual VisualEffects getCurrentEffects() const = 0;
    virtual DesktopSettings getCurrentDesktopSettings() const = 0;
};

} // namespace rex::desktop_personalization
