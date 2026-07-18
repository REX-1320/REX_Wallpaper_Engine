#include "shared/UUID.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace rex::shared {

UUID::UUID() : m_bytes{} {}

UUID::UUID(const std::array<uint8_t, 16>& bytes) : m_bytes(bytes) {}

UUID UUID::Generate() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::array<uint8_t, 16> bytes;
    uint64_t* words = reinterpret_cast<uint64_t*>(bytes.data());
    words[0] = dis(gen);
    words[1] = dis(gen);

    // Version 4 (random)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Variant (RFC 4122)
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    return UUID(bytes);
}

UUID UUID::Parse(const std::string& str) {
    UUID uuid;
    std::string clean = str;
    clean.erase(std::remove(clean.begin(), clean.end(), '-'), clean.end());

    if (clean.size() != 32) {
        return uuid;
    }

    for (int i = 0; i < 32; i += 2) {
        std::string byteStr = clean.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
        uuid.m_bytes[i / 2] = byte;
    }

    return uuid;
}

std::string UUID::ToString() const {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');

    for (int i = 0; i < 16; i++) {
        ss << std::setw(2) << static_cast<int>(m_bytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << '-';
        }
    }

    return ss.str();
}

bool UUID::IsValid() const {
    for (uint8_t byte : m_bytes) {
        if (byte != 0) return true;
    }
    return false;
}

bool UUID::operator==(const UUID& other) const {
    return m_bytes == other.m_bytes;
}

bool UUID::operator!=(const UUID& other) const {
    return !(*this == other);
}

} // namespace rex::shared
