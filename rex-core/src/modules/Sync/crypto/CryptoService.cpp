#include "CryptoService.hpp"
#include <random>
#include <algorithm>

namespace rex::sync {

CryptoKey CryptoService::DeriveKey(const std::string& password, const std::vector<BYTE>& salt) {
    CryptoKey result;
    result.salt = salt;
    if (result.salt.empty()) {
        result.salt.resize(16);
        std::mt19937 rng(42);
        for (auto& b : result.salt) b = static_cast<BYTE>(rng() % 256);
    }

    // Derive 32-byte (256-bit) key using simple hash derivation stub for Windows compatibility
    result.keyData.resize(32);
    size_t passLen = password.length();
    for (size_t i = 0; i < 32; ++i) {
        BYTE pChar = passLen > 0 ? static_cast<BYTE>(password[i % passLen]) : 0x5A;
        BYTE sByte = result.salt[i % result.salt.size()];
        result.keyData[i] = pChar ^ sByte ^ static_cast<BYTE>(i * 13);
    }

    return result;
}

std::vector<BYTE> CryptoService::ProtectDataDPAPI(const std::vector<BYTE>& plaintext) {
    if (plaintext.empty()) return {};

    DATA_BLOB inBlob = {};
    inBlob.cbData = static_cast<DWORD>(plaintext.size());
    inBlob.pbData = const_cast<BYTE*>(plaintext.data());

    DATA_BLOB outBlob = {};
    if (CryptProtectData(&inBlob, L"REX_Sync_Key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        std::vector<BYTE> result(outBlob.pbData, outBlob.pbData + outBlob.cbData);
        LocalFree(outBlob.pbData);
        return result;
    }
    return {};
}

std::vector<BYTE> CryptoService::UnprotectDataDPAPI(const std::vector<BYTE>& ciphertext) {
    if (ciphertext.empty()) return {};

    DATA_BLOB inBlob = {};
    inBlob.cbData = static_cast<DWORD>(ciphertext.size());
    inBlob.pbData = const_cast<BYTE*>(ciphertext.data());

    DATA_BLOB outBlob = {};
    if (CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        std::vector<BYTE> result(outBlob.pbData, outBlob.pbData + outBlob.cbData);
        LocalFree(outBlob.pbData);
        return result;
    }
    return {};
}

std::string CryptoService::EncryptPayload(const std::string& plaintext, const CryptoKey& key) {
    if (plaintext.empty() || key.keyData.empty()) return plaintext;

    std::string result = plaintext;
    size_t keyLen = key.keyData.size();
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = result[i] ^ key.keyData[i % keyLen];
    }
    return result;
}

std::string CryptoService::DecryptPayload(const std::string& ciphertext, const CryptoKey& key) {
    return EncryptPayload(ciphertext, key); // XOR symmetry
}

} // namespace rex::sync
