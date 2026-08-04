#include "SyncStore.hpp"
#include <algorithm>

namespace rex::sync {

SyncStore::SyncStore() {
    // Initialize default local device version vector entry
    m_versionVectors["local-device-uuid"] = 1;
}

void SyncStore::PutObject(const SyncObject& obj) {
    std::string keyPath = obj.collection + ":" + obj.key;
    m_store[keyPath] = obj;
    if (obj.version > m_versionVectors[obj.deviceId]) {
        m_versionVectors[obj.deviceId] = obj.version;
    }
}

bool SyncStore::GetItem(const std::string& collection, const std::string& key, SyncObject& outObj) const {
    std::string keyPath = collection + ":" + key;
    auto it = m_store.find(keyPath);
    if (it != m_store.end()) {
        outObj = it->second;
        return true;
    }
    return false;
}

std::vector<SyncObject> SyncStore::GetCollection(const std::string& collection) const {
    std::vector<SyncObject> result;
    std::string prefix = collection + ":";
    for (const auto& [k, v] : m_store) {
        if (k.rfind(prefix, 0) == 0) {
            result.push_back(v);
        }
    }
    return result;
}

void SyncStore::SetDeviceVersion(const std::string& deviceId, uint64_t version) {
    m_versionVectors[deviceId] = version;
}

uint64_t SyncStore::GetDeviceVersion(const std::string& deviceId) const {
    auto it = m_versionVectors.find(deviceId);
    if (it != m_versionVectors.end()) return it->second;
    return 0;
}

void SyncStore::LogConflict(const SyncConflict& conflict) {
    // Avoid duplicates
    auto it = std::find_if(m_conflicts.begin(), m_conflicts.end(), [&](const SyncConflict& c) {
        return c.collection == conflict.collection && c.key == conflict.key;
    });
    if (it != m_conflicts.end()) {
        *it = conflict;
    } else {
        m_conflicts.push_back(conflict);
    }
}

void SyncStore::ResolveConflict(const std::string& collection, const std::string& key) {
    m_conflicts.erase(
        std::remove_if(m_conflicts.begin(), m_conflicts.end(), [&](const SyncConflict& c) {
            return c.collection == collection && c.key == key;
        }),
        m_conflicts.end()
    );
}

} // namespace rex::sync
