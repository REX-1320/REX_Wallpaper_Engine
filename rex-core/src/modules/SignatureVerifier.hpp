#pragma once
#include <string>
#include <unordered_map>
#include <memory>

namespace rex::modules {

class SignatureVerifier {
public:
    SignatureVerifier();
    ~SignatureVerifier();

    void addTrustedKey(const std::string& keyId, const std::string& publicKeyPem);
    bool verifyFile(const std::string& filePath, const std::string& signatureHex,
                    const std::string& keyId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rex::modules
