#pragma once

#include <string>
#include <cstdint>
#include <array>

namespace rex::shared {

class UUID {
public:
    UUID();
    explicit UUID(const std::array<uint8_t, 16>& bytes);

    static UUID Generate();
    static UUID Parse(const std::string& str);

    std::string ToString() const;
    std::array<uint8_t, 16> ToBytes() const { return m_bytes; }

    bool IsValid() const;

    bool operator==(const UUID& other) const;
    bool operator!=(const UUID& other) const;

private:
    std::array<uint8_t, 16> m_bytes;
};

} // namespace rex::shared
