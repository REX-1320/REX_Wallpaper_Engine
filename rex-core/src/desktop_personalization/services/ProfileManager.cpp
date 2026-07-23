#include "ProfileManager.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace rex::desktop_personalization {

ProfileManager::ProfileManager(std::shared_ptr<rex::modules::ConfigurationStore> configStore,
                               std::shared_ptr<rex::modules::EventBus> bus)
    : configStore_(std::move(configStore)), bus_(std::move(bus)) {}

std::vector<Profile> ProfileManager::listProfiles() const {
    auto configs = configStore_->loadAll();
    std::vector<Profile> profiles;
    for (const auto& cfg : configs) {
        if (cfg.moduleId == "rex.desktop_personalization") {
            Profile p;
            p.name = cfg.moduleId;
            profiles.push_back(p);
        }
    }
    return profiles;
}

Profile ProfileManager::loadProfile(const std::string& name) const {
    Profile p;
    auto cfg = configStore_->loadModule(name);
    p.name = cfg.moduleId;
    return p;
}

bool ProfileManager::saveProfile(const Profile& profile) {
    rex::modules::ModuleConfig cfg;
    cfg.moduleId = profile.name;
    cfg.enabled = true;
    configStore_->saveModule(cfg);

    bus_->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::ProfileSaved,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
    return true;
}

bool ProfileManager::deleteProfile(const std::string& name) {
    auto configs = configStore_->loadAll();
    auto it = std::remove_if(configs.begin(), configs.end(),
        [&](const rex::modules::ModuleConfig& c) { return c.moduleId == name; });
    if (it == configs.end()) return false;
    configs.erase(it, configs.end());
    configStore_->saveAll(configs);

    bus_->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::ProfileDeleted,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
    return true;
}

bool ProfileManager::exportProfile(const std::string& name, const std::string& filePath) const {
    Profile p = loadProfile(name);
    std::ofstream file(filePath, std::ios::binary);
    if (!file) return false;
    file << "profile:" << p.name << "\n";
    file << "version:" << p.version << "\n";
    return file.good();
}

bool ProfileManager::importProfile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file) return false;

    Profile p;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("profile:", 0) == 0) {
            p.name = line.substr(8);
        } else if (line.rfind("version:", 0) == 0) {
            p.version = std::stoi(line.substr(8));
        }
    }
    return saveProfile(p);
}

bool ProfileManager::rollbackToLastGood() {
    bus_->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::RollbackApplied,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
    return true;
}

} // namespace rex::desktop_personalization
