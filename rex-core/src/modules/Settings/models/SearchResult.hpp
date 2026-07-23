#pragma once
#include "../registry/Setting.hpp"

namespace rex::settings {
struct SearchResult {
    Setting setting;
    double relevance = 0.0;
};
}
