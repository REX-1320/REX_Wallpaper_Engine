#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace rex::sync {

struct SyncObject {
    std::string collection;  ///< e.g. "settings", "profiles", "wallpaper_library", "widget_instances"
    std::string key;         ///< Unique item identifier
    std::string value;       ///< Encrypted payload value
    uint64_t version = 1;    ///< Monotonic item version
    std::string lastModified;///< UTC ISO-8601 timestamp
    std::string deviceId;    ///< Origin device UUID
};

struct SyncConflict {
    std::string collection;
    std::string key;
    std::string localValue;
    std::string remoteValue;
    std::string localTimestamp;
    std::string remoteTimestamp;
};

/// SyncStore maintains local change tracking, version vectors (deviceId -> maxVersion),
/// and the unresolved conflict log.
class SyncStore {
public:
    SyncStore();
    ~SyncStore() = default;

    /// Stores or updates a sync object locally.
    void PutObject(const SyncObject& obj);

    /// Gets object by collection and key.
    bool GetItem(const std::string& collection, const std::string& key, SyncObject& outObj) const;

    /// Returns all objects in a collection.
    std::vector<SyncObject> GetCollection(const std::string& collection) const;

    /// Updates version vector for a device.
    void SetDeviceVersion(const std::string& deviceId, uint64_t version);

    /// Gets max version for a device.
    uint64_t GetDeviceVersion(const std::string& deviceId) const;

    /// Logs a sync conflict to conflicts queue.
    void LogConflict(const SyncConflict& conflict);

    /// Returns all pending unresolved conflicts.
    std::vector<SyncConflict> GetConflicts() const { return m_conflicts; }

    /// Clears a resolved conflict.
    void ResolveConflict(const std::string& collection, const std::string& key);

private:
    std::unordered_map<std::string, SyncObject> m_store; // "collection:key" -> SyncObject
    std::unordered_map<std::string, uint64_t> m_versionVectors; // deviceId -> maxVersion
    std::vector<SyncConflict> m_conflicts;
};

} // namespace rex::sync
