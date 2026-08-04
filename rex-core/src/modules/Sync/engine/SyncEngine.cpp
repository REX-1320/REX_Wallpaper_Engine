#include "SyncEngine.hpp"

namespace rex::sync {

SyncEngine::SyncEngine() {
    m_state = SyncState::SignedOut;
}

bool SyncEngine::SignIn(const std::string& email, const std::string& password) {
    auto auth = m_restClient.Login(email, password);
    if (auth.success) {
        m_userEmail = email;
        m_authToken = auth.token;
        std::vector<BYTE> salt = { 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8 };
        m_userKey = CryptoService::DeriveKey(password, salt);
        m_state = SyncState::UpToDate;
        SyncNow();
        return true;
    }
    m_state = SyncState::Error;
    return false;
}

void SyncEngine::SignOut() {
    m_userEmail.clear();
    m_authToken.clear();
    m_userKey = {};
    m_state = SyncState::SignedOut;
}

bool SyncEngine::SyncNow() {
    if (m_state == SyncState::SignedOut || m_authToken.empty()) return false;

    m_state = SyncState::Syncing;
    PerformSyncPass();

    if (!m_store.GetConflicts().empty()) {
        m_state = SyncState::ConflictPending;
    } else {
        m_state = SyncState::UpToDate;
    }
    return true;
}

void SyncEngine::Update(float deltaTime) {
    if (m_state == SyncState::SignedOut) return;

    m_timer += deltaTime;
    float intervalSeconds = static_cast<float>(m_settings.syncIntervalMinutes * 60);
    if (m_timer >= intervalSeconds) {
        m_timer = 0.0f;
        SyncNow();
    }
}

void SyncEngine::PerformSyncPass() {
    // 1. Push local changes per enabled collection
    if (m_settings.syncSettings) {
        auto settings = m_store.GetCollection("settings");
        if (!settings.empty()) {
            m_restClient.PushObjects(m_authToken, settings);
        }
    }
    if (m_settings.syncProfiles) {
        auto profiles = m_store.GetCollection("profiles");
        if (!profiles.empty()) {
            m_restClient.PushObjects(m_authToken, profiles);
        }
    }

    // 2. Pull remote changes
    if (m_settings.syncSettings) {
        auto remoteSettings = m_restClient.PullObjects(m_authToken, "settings", m_store.GetDeviceVersion("remote"));
        for (const auto& remoteObj : remoteSettings) {
            SyncObject localObj;
            if (m_store.GetItem(remoteObj.collection, remoteObj.key, localObj)) {
                // Conflict check (Last-Writer-Wins)
                if (localObj.version != remoteObj.version && localObj.value != remoteObj.value) {
                    SyncConflict conflict;
                    conflict.collection = remoteObj.collection;
                    conflict.key = remoteObj.key;
                    conflict.localValue = localObj.value;
                    conflict.remoteValue = remoteObj.value;
                    conflict.localTimestamp = localObj.lastModified;
                    conflict.remoteTimestamp = remoteObj.lastModified;
                    m_store.LogConflict(conflict);
                } else {
                    m_store.PutObject(remoteObj);
                }
            } else {
                m_store.PutObject(remoteObj);
            }
        }
    }
}

} // namespace rex::sync
