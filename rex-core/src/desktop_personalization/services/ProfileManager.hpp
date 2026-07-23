#pragma once
#include <string>
#include <vector>
#include <memory>
#include "../models/Profile.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"

namespace rex::desktop_personalization {

class ProfileManager {
public:
    ProfileManager(std::shared_ptr<rex::modules::ConfigurationStore> configStore,
                   std::shared_ptr<rex::modules::EventBus> bus);

    std::vector<Profile> listProfiles() const;
    Profile loadProfile(const std::string& name) const;
    bool saveProfile(const Profile& profile);
    bool deleteProfile(const std::string& name);
    bool exportProfile(const std::string& name, const std::string& filePath) const;
    bool importProfile(const std::string& filePath);

    bool rollbackToLastGood();

private:
    std::shared_ptr<rex::modules::ConfigurationStore> configStore_;
    std::shared_ptr<rex::modules::EventBus> bus_;
    Profile lastGoodProfile_;
};

} // namespace rex::desktop_personalization
