#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "../models/Profile.hpp"

namespace rex::profiles {

class IProfileService {
public:
    virtual ~IProfileService() = default;

    virtual std::vector<Profile> listProfiles() const = 0;
    virtual bool createProfile(const Profile& profile) = 0;
    virtual bool updateProfile(const Profile& profile) = 0;
    virtual bool deleteProfile(const std::string& id) = 0;
    virtual Profile getProfile(const std::string& id) const = 0;

    virtual bool applyProfile(const std::string& id) = 0;
    virtual bool applyProfile(const Profile& profile) = 0;

    virtual void startAutoSwitch() = 0;
    virtual void stopAutoSwitch() = 0;
    virtual bool isAutoSwitching() const = 0;

    using ProfileCallback = std::function<void(const std::string& profileId)>;
    virtual void onProfileApplied(ProfileCallback cb) = 0;
};

} // namespace rex::profiles
