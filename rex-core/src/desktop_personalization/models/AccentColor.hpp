#pragma once
#include <cstdint>

namespace rex::desktop_personalization {

struct AccentColor {
    uint8_t r = 0, g = 120, b = 215, a = 255;

    static AccentColor defaultAccent() { return {}; }

    bool operator==(const AccentColor& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const AccentColor& o) const { return !(*this == o); }
};

} // namespace rex::desktop_personalization
