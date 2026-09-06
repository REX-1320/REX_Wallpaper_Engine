#include "RuntimeSettings.hpp"
#include "Json.hpp"
#include <fstream>
#include <sstream>
#include <shared/Logger.hpp>

namespace rex::wallpaper {

RuntimeSettings::RuntimeSettings(const std::filesystem::path& file)
    : m_file(file) {
    Load();
}

bool RuntimeSettings::Load() {
    std::lock_guard lock(m_mutex);
    m_data.clear();
    if (!std::filesystem::exists(m_file)) return false;
    std::ifstream in(m_file);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();

    auto json = rex::runtime::Json::Parse(ss.str());
    if (!json || !json->IsObject()) return false;

    for (const auto& [key, value] : json->AsObject()) {
        if (value.IsString()) {
            m_data[key] = value.AsString();
        }
    }
    return true;
}

bool RuntimeSettings::Save() {
    std::lock_guard lock(m_mutex);

    rex::runtime::Json::Object obj;
    for (const auto& [k, v] : m_data) {
        obj.emplace(k, rex::runtime::Json(v));
    }
    std::string json = rex::runtime::Json(std::move(obj)).Dump();

    const std::string tmpPath = m_file.string() + ".tmp";
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out.is_open()) return false;
    out << json;
    out.close();
    std::error_code ec;
    std::filesystem::rename(tmpPath, m_file, ec);
    return !ec;
}

void RuntimeSettings::SetMonitorWallpaper(uint64_t edidHash, const std::string& path) {
    SetString("monitor." + std::to_string(edidHash) + ".wallpaper", path);
}

std::string RuntimeSettings::GetMonitorWallpaper(uint64_t edidHash) const {
    return GetString("monitor." + std::to_string(edidHash) + ".wallpaper");
}

void RuntimeSettings::RemoveMonitorWallpaper(uint64_t edidHash) {
    std::lock_guard lock(m_mutex);
    m_data.erase("monitor." + std::to_string(edidHash) + ".wallpaper");
    Save();
}

void RuntimeSettings::SetString(const std::string& key, const std::string& value) {
    std::lock_guard lock(m_mutex);
    m_data[key] = value;
    Save();
}

std::string RuntimeSettings::GetString(const std::string& key, const std::string& def) const {
    std::lock_guard lock(m_mutex);
    auto it = m_data.find(key);
    return it != m_data.end() ? it->second : def;
}

void RuntimeSettings::RemoveKey(const std::string& key) {
    std::lock_guard lock(m_mutex);
    m_data.erase(key);
    Save();
}

std::vector<std::string> RuntimeSettings::GetKeysByPrefix(const std::string& prefix) const {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [k, v] : m_data) {
        if (k.rfind(prefix, 0) == 0) {
            result.push_back(k);
        }
    }
    return result;
}

} // namespace rex::wallpaper
