#include "ProfileManager.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

namespace rex::profiles {

static std::string encodeProfiles(const std::vector<Profile>& profiles) {
    std::ostringstream ss;
    for (auto& p : profiles) {
        ss << "BEGIN\n";
        ss << "id=" << p.id << "\n";
        ss << "name=" << p.name << "\n";
        ss << "desc=" << p.description << "\n";
        ss << "wall=" << p.wallpaperId << "\n";
        ss << "theme=" << p.themeId << "\n";
        for (auto& w : p.widgetIds) ss << "widget=" << w << "\n";
        for (auto& [k, v] : p.personalizationSettings)
            ss << "ps=" << k << "|" << v << "\n";
        ss << "END\n";
    }
    return ss.str();
}

static std::vector<Profile> decodeProfiles(const std::string& data) {
    std::vector<Profile> profiles;
    std::istringstream ss(data);
    std::string line;
    Profile current;
    bool inProfile = false;
    while (std::getline(ss, line)) {
        if (line == "BEGIN") {
            current = Profile();
            inProfile = true;
        } else if (line == "END") {
            if (inProfile) {
                current.created = std::chrono::system_clock::now();
                current.modified = current.created;
                profiles.push_back(std::move(current));
            }
            inProfile = false;
        } else if (inProfile) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "id") current.id = val;
            else if (key == "name") current.name = val;
            else if (key == "desc") current.description = val;
            else if (key == "wall") current.wallpaperId = val;
            else if (key == "theme") current.themeId = val;
            else if (key == "widget") current.widgetIds.push_back(val);
            else if (key == "ps") {
                auto pipe = val.find('|');
                if (pipe != std::string::npos)
                    current.personalizationSettings[val.substr(0, pipe)] = val.substr(pipe + 1);
            }
        }
    }
    return profiles;
}

ProfileManager::ProfileManager(
    std::shared_ptr<rex::modules::ConfigurationStore> configStore,
    std::shared_ptr<rex::modules::EventBus> eventBus,
    std::shared_ptr<rex::settings::ISettingsService> settingsService,
    std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
    std::shared_ptr<rex::widgets::IWidgetService> widgetService,
    std::shared_ptr<rex::personalization::IPersonalizationService> personalizationService)
    : m_configStore(configStore), m_eventBus(eventBus),
      m_settingsService(settingsService),
      m_wallpaperEngine(wallpaperEngine), m_widgetService(widgetService),
      m_personalizationService(personalizationService)
{
    if (m_settingsService) {
        rex::settings::Setting profilesData;
        profilesData.id = "profiles.data";
        profilesData.name = "Profiles Data";
        profilesData.description = "Stored profile configurations";
        profilesData.category = "profiles";
        profilesData.type = rex::settings::SettingType::String;
        profilesData.defaultValue = std::string("");
        profilesData.currentValue = std::string("");
        m_settingsService->registerSetting(profilesData);
    }
}

ProfileManager::~ProfileManager() {
    stopAutoSwitch();
}

std::vector<Profile> ProfileManager::loadAll() const {
    if (!m_settingsService) return {};
    auto val = m_settingsService->getValue("profiles.data");
    if (!val.has_value()) return {};
    if (!std::holds_alternative<std::string>(val.value())) return {};
    return decodeProfiles(std::get<std::string>(val.value()));
}

void ProfileManager::saveAll(const std::vector<Profile>& profiles) const {
    if (!m_settingsService) return;
    m_settingsService->setValue("profiles.data", encodeProfiles(profiles));
}

std::vector<Profile> ProfileManager::listProfiles() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return loadAll();
}

bool ProfileManager::createProfile(const Profile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto profiles = loadAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
        [&](auto& p) { return p.id == profile.id; });
    if (it != profiles.end()) return false;
    auto p = profile;
    p.created = std::chrono::system_clock::now();
    p.modified = p.created;
    profiles.push_back(std::move(p));
    saveAll(profiles);
    if (m_eventBus) {
        m_eventBus->publish(rex::modules::ModuleEvent{
            rex::modules::ModuleEventType::ProfileSaved, "profiles"});
    }
    return true;
}

bool ProfileManager::updateProfile(const Profile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto profiles = loadAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
        [&](auto& p) { return p.id == profile.id; });
    if (it == profiles.end()) return false;
    *it = profile;
    it->modified = std::chrono::system_clock::now();
    saveAll(profiles);
    return true;
}

bool ProfileManager::deleteProfile(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto profiles = loadAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
        [&](auto& p) { return p.id == id; });
    if (it == profiles.end()) return false;
    profiles.erase(it);
    saveAll(profiles);
    if (m_eventBus) {
        m_eventBus->publish(rex::modules::ModuleEvent{
            rex::modules::ModuleEventType::ProfileDeleted, "profiles"});
    }
    return true;
}

Profile ProfileManager::getProfile(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto profiles = loadAll();
    auto it = std::find_if(profiles.begin(), profiles.end(),
        [&](auto& p) { return p.id == id; });
    if (it != profiles.end()) return *it;
    return {};
}

bool ProfileManager::applyProfile(const std::string& id) {
    auto p = getProfile(id);
    if (p.id.empty()) return false;
    return applyProfile(p);
}

bool ProfileManager::applyProfile(const Profile& profile) {
    if (m_widgetService && !profile.widgetIds.empty()) {
        for (auto& wid : profile.widgetIds) {
            m_widgetService->createWidget(wid, 100, 100);
        }
    }

    if (m_personalizationService) {
        for (auto& [key, val] : profile.personalizationSettings) {
            if (key == "taskbar.position") {
                auto ts = m_personalizationService->getTaskbarSettings();
                // A non-numeric value (hand-edited or corrupted settings file)
                // used to throw out of applyProfile; on the auto-switch thread
                // that is an uncaught exception -> std::terminate, killing the
                // whole host instead of skipping one setting.
                try {
                    size_t consumed = 0;
                    const int parsed = std::stoi(val, &consumed);
                    if (consumed == val.size()) {
                        ts.position = parsed;
                    } else {
                        std::cerr << "[ProfileManager] ignoring non-numeric "
                                     "taskbar.position value: " << val << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[ProfileManager] ignoring invalid "
                                 "taskbar.position '" << val << "': "
                              << e.what() << "\n";
                }
                m_personalizationService->setTaskbarSettings(ts);
            } else if (key == "taskbar.autoHide") {
                auto ts = m_personalizationService->getTaskbarSettings();
                ts.autoHide = (val == "true");
                m_personalizationService->setTaskbarSettings(ts);
            } else if (key == "taskbar.transparent") {
                auto ts = m_personalizationService->getTaskbarSettings();
                ts.transparent = (val == "true");
                m_personalizationService->setTaskbarSettings(ts);
            } else if (key == "explorer.mica") {
                auto es = m_personalizationService->getExplorerSettings();
                es.mica = (val == "true");
                m_personalizationService->setExplorerSettings(es);
            } else if (key == "start.replace") {
                auto ss = m_personalizationService->getStartMenuSettings();
                ss.replaceStart = (val == "true");
                m_personalizationService->setStartMenuSettings(ss);
            }
        }
    }

    if (m_appliedCallback) m_appliedCallback(profile.id);

    if (m_eventBus) {
        m_eventBus->publish(rex::modules::ModuleEvent{
            rex::modules::ModuleEventType::ProfileLoaded, "profiles"});
    }
    return true;
}

void ProfileManager::startAutoSwitch() {
    if (m_autoSwitching) return;
    m_autoSwitching = true;
    m_autoSwitchThread = std::thread([this]() { autoSwitchLoop(); });
}

void ProfileManager::stopAutoSwitch() {
    m_autoSwitching = false;
    if (m_autoSwitchThread.joinable()) m_autoSwitchThread.join();
}

bool ProfileManager::isAutoSwitching() const {
    return m_autoSwitching.load();
}

void ProfileManager::autoSwitchLoop() {
    while (m_autoSwitching) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!m_autoSwitching) break;

        // Collect matches under the lock, apply outside it. The old loop
        // unlocked in place and then kept iterating: the next matching profile
        // unlocked an already-unlocked unique_lock (UB — mutex unlocked twice)
        // and applied profiles while holding no snapshot.
        std::vector<std::string> matches;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (auto& p : loadAll()) {
                for (auto& t : p.triggers) {
                    if (t.enabled && evaluateTrigger(t)) {
                        matches.push_back(p.id);
                        break;
                    }
                }
            }
        }

        for (const auto& id : matches) {
            if (!m_autoSwitching) break;
            // A failing profile must not kill this thread (an exception here
            // would escape the thread function and abort the process).
            try {
                applyProfile(id);
            } catch (const std::exception& e) {
                std::cerr << "[ProfileManager] auto-switch to '" << id
                          << "' failed: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[ProfileManager] auto-switch to '" << id
                          << "' failed with an unknown error\n";
            }
        }
    }
}

bool ProfileManager::evaluateTrigger(const ProfileTrigger& trigger) const {
    switch (trigger.type) {
        case TriggerType::Manual: return false;
        case TriggerType::TimeRange: return true;
        case TriggerType::Process: return true;
        case TriggerType::Network: return true;
        case TriggerType::SystemState: return true;
    }
    return false;
}

} // namespace rex::profiles
