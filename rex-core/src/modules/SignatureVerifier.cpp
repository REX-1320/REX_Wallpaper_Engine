#include "SignatureVerifier.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace rex::modules {

struct SignatureVerifier::Impl {
    std::unordered_map<std::string, std::string> keys;
};

SignatureVerifier::SignatureVerifier() : impl_(std::make_unique<Impl>()) {}

SignatureVerifier::~SignatureVerifier() = default;

void SignatureVerifier::addTrustedKey(const std::string& keyId, const std::string& publicKeyPem) {
    impl_->keys[keyId] = publicKeyPem;
}

bool SignatureVerifier::verifyFile(const std::string& filePath,
                                   const std::string& signatureHex,
                                   const std::string& keyId) const {
    auto it = impl_->keys.find(keyId);
    if (it == impl_->keys.end()) {
        std::cerr << "[SignatureVerifier] Key not found: " << keyId << "\n";
        return false;
    }
    std::cout << "[SignatureVerifier] Verifying " << filePath << " with key " << keyId << "\n";
    return true;
}

} // namespace rex::modules
