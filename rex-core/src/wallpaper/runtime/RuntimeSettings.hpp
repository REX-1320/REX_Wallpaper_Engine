#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <filesystem>

namespace rex::wallpaper {

// Lightweight persistent JSON settings for the runtime (no full Settings
// service). Stores per-monitor wallpaper assignments and playlist data as a
// flat string→string map, persisted to a small JSON file. Replaces the heavy
// Settings Framework (registry + validation + search + history + backup +
// schema + import/export) which added 20–30 MB.
class RuntimeSettings {
public:
    explicit RuntimeSettings(const std::filesystem::path& file);
    ~RuntimeSettings() = default;

    RuntimeSettings(const RuntimeSettings&) = delete;
    RuntimeSettings& operator=(const RuntimeSettings&) = delete;

    bool Load();
    bool Save();

    // Per-monitor wallpaper (EDID-keyed)
    void SetMonitorWallpaper(uint64_t edidHash, const std::string& path);
    std::string GetMonitorWallpaper(uint64_t edidHash) const;
    void RemoveMonitorWallpaper(uint64_t edidHash);

    // Playlist persistence
    void SetString(const std::string& key, const std::string& value);
    std::string GetString(const std::string& key, const std::string& def = "") const;
    void RemoveKey(const std::string& key);

    // Return all keys matching a prefix (for garbage-collecting stale entries).
    std::vector<std::string> GetKeysByPrefix(const std::string& prefix) const;

private:
    std::filesystem::path m_file;
    mutable std::recursive_mutex m_mutex;
    std::unordered_map<std::string, std::string> m_data;
};

} // namespace rex::wallpaper
