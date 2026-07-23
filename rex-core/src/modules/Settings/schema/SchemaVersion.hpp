#pragma once
#include <cstdint>

namespace rex::settings {
struct SchemaVersion {
    int major = 1;
    int minor = 0;
    bool operator<(const SchemaVersion& o) const {
        return (major != o.major) ? major < o.major : minor < o.minor;
    }
    bool operator==(const SchemaVersion&) const = default;
    bool operator!=(const SchemaVersion& o) const { return !(*this == o); }
};
}
