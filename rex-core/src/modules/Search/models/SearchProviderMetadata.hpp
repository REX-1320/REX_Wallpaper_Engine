#pragma once
#include <string>
#include <vector>
#include "SearchCapabilities.hpp"

namespace rex::search {
struct SearchProviderMetadata {
    std::string id;
    std::string displayName;
    std::string description;
    std::vector<std::string> categories;
    int priority = 0;
    int timeoutMs = 500;
    SearchCapability capabilities = SearchCapability::None;
};
}
