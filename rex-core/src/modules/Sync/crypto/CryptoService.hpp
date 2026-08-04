#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <string>
#include <vector>

namespace rex::sync {

struct CryptoKey {
    std::vector<BYTE> keyData;
    std::vector<BYTE> salt;
};

/// CryptoService handles client-side encryption and decryption of sync payloads
/// using AES-256-GCM, PBKDF2 key derivation from user credentials, and DPAPI key storage.
class CryptoService {
public:
    CryptoService() = default;
    ~CryptoService() = default;

    /// Derives AES-256 key using PBKDF2-HMAC-SHA256 from user password and salt.
    static CryptoKey DeriveKey(const std::string& password, const std::vector<BYTE>& salt);

    /// Protects sensitive bytes using Windows DPAPI (CryptProtectData).
    static std::vector<BYTE> ProtectDataDPAPI(const std::vector<BYTE>& plaintext);

    /// Unprotects DPAPI-encrypted bytes (CryptUnprotectData).
    static std::vector<BYTE> UnprotectDataDPAPI(const std::vector<BYTE>& ciphertext);

    /// Encrypts plaintext payload string using AES-256 key.
    static std::string EncryptPayload(const std::string& plaintext, const CryptoKey& key);

    /// Decrypts ciphertext payload string using AES-256 key.
    static std::string DecryptPayload(const std::string& ciphertext, const CryptoKey& key);
};

} // namespace rex::sync
