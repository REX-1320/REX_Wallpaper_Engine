#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include "../crypto/CryptoService.hpp"
#include "../storage/SyncStore.hpp"
#include "../transport/SyncRestClient.hpp"

namespace rex::sync {

enum class SyncState {
    SignedOut,
    Syncing,
    UpToDate,
    ConflictPending,
    Error
};

struct SyncSettings {
    bool syncProfiles = true;
    bool syncSettings = true;
    bool syncWallpapers = true;
    bool syncWidgets = true;
    bool syncThemes = true;
    bool syncMarketplace = true;
    bool wifiOnlyForFiles = true;
    uint32_t syncIntervalMinutes = 5;
};

/// SyncEngine manages change tracking, background push/pull cycles,
/// conflict resolution (LWW), and selective category toggles.
class SyncEngine {
public:
    SyncEngine();
    ~SyncEngine() = default;

    /// Signs in user and initializes local encryption key.
    bool SignIn(const std::string& email, const std::string& password);

    /// Signs out and clears local user key.
    void SignOut();

    /// Triggers an immediate manual sync pass ("Sync Now").
    bool SyncNow();

    /// Updates background loop state.
    void Update(float deltaTime);

    /// Returns current state.
    SyncState GetState() const { return m_state; }

    /// Returns current user email.
    std::string GetUserEmail() const { return m_userEmail; }

    /// Returns sync settings reference.
    SyncSettings& GetSettings() { return m_settings; }
    const SyncSettings& GetSettings() const { return m_settings; }

    /// Access to internal store.
    SyncStore& GetStore() { return m_store; }

private:
    SyncState m_state = SyncState::SignedOut;
    std::string m_userEmail;
    std::string m_authToken;
    CryptoKey m_userKey;
    SyncSettings m_settings;
    SyncStore m_store;
    SyncRestClient m_restClient;

    float m_timer = 0.0f;
    void PerformSyncPass();
};

} // namespace rex::sync
