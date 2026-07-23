#pragma once
#include <string>
#include <vector>

namespace rex::settings {

struct Category {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> subCategories;
};

} // namespace rex::settings
