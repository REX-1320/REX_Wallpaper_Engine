#pragma once
#include <string>

namespace rex::settings {

struct ValidationResult {
    bool valid = true;
    std::string errorMessage;
};

} // namespace rex::settings
