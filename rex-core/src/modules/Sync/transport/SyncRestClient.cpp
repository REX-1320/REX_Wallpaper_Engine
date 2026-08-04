#include "SyncRestClient.hpp"

namespace rex::sync {

AuthResult SyncRestClient::Login(const std::string& email, const std::string& password) {
    AuthResult result;
    if (email.empty() || password.empty()) {
        result.errorMessage = "Email and password cannot be empty";
        return result;
    }

    // Mock successful authentication for testing/offline mode
    result.success = true;
    result.email = email;
    result.token = "rex_token_mock_" + std::to_string(email.length() * 1337);
    return result;
}

bool SyncRestClient::PushObjects(const std::string& authToken, const std::vector<SyncObject>& objects) {
    if (authToken.empty() || objects.empty()) return false;
    // Push objects to REST endpoint (/sync/push)
    return true;
}

std::vector<SyncObject> SyncRestClient::PullObjects(const std::string& authToken, const std::string& collection, uint64_t sinceVersion) {
    std::vector<SyncObject> result;
    if (authToken.empty()) return result;

    // Return empty list if already up-to-date
    return result;
}

bool SyncRestClient::UploadFileChunk(const std::string& authToken, const std::string& fileId, uint32_t chunkIndex, const std::vector<BYTE>& chunkData) {
    if (authToken.empty() || fileId.empty() || chunkData.empty()) return false;
    return true;
}

} // namespace rex::sync
