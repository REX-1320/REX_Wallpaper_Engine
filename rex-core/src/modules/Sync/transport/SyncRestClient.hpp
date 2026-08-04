#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "../storage/SyncStore.hpp"

namespace rex::sync {

struct AuthResult {
    bool success = false;
    std::string token;
    std::string email;
    std::string errorMessage;
};

/// SyncRestClient provides REST communication with REX Cloud endpoints
/// for authentication, object push/pull, and file chunk transfer.
class SyncRestClient {
public:
    SyncRestClient() = default;
    ~SyncRestClient() = default;

    /// Authenticates with user credentials.
    AuthResult Login(const std::string& email, const std::string& password);

    /// Pushes updated local sync objects to cloud.
    bool PushObjects(const std::string& authToken, const std::vector<SyncObject>& objects);

    /// Pulls remote sync objects since specified version vector.
    std::vector<SyncObject> PullObjects(const std::string& authToken, const std::string& collection, uint64_t sinceVersion);

    /// Uploads a file chunk for wallpaper/widget package asset sync.
    bool UploadFileChunk(const std::string& authToken, const std::string& fileId, uint32_t chunkIndex, const std::vector<BYTE>& chunkData);

    /// Sets cloud endpoint base URL.
    void SetBaseUrl(const std::string& url) { m_baseUrl = url; }
    std::string GetBaseUrl() const { return m_baseUrl; }

private:
    std::string m_baseUrl = "https://api.rex.desktop/v1";
};

} // namespace rex::sync
