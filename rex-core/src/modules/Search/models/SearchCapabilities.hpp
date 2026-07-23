#pragma once
#include <cstdint>

namespace rex::search {
enum class SearchCapability : uint32_t {
    None        = 0,
    Fuzzy       = 1 << 0,
    Regex       = 1 << 1,
    Tags        = 1 << 2,
    Prefix      = 1 << 3,
    Semantic    = 1 << 4,
    Boolean     = 1 << 5
};

inline SearchCapability operator|(SearchCapability a, SearchCapability b) {
    return static_cast<SearchCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasCapability(SearchCapability caps, SearchCapability flag) {
    return (static_cast<uint32_t>(caps) & static_cast<uint32_t>(flag)) != 0;
}
}
