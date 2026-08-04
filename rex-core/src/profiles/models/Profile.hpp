#pragma once
#include <string>
#include <vector>
#include <map>
#include <chrono>

namespace rex::profiles {

enum class TriggerType { Manual, TimeRange, Process, Network, SystemState };

struct ProfileTrigger {
    std::string id;
    TriggerType type = TriggerType::Manual;
    std::string config;
    bool enabled = true;
};

struct Profile {
    std::string id;
    std::string name;
    std::string description;
    std::string wallpaperId;
    std::string themeId;
    std::vector<std::string> widgetIds;
    std::map<std::string, std::string> personalizationSettings;
    std::vector<ProfileTrigger> triggers;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point modified;
};

} // namespace rex::profiles
