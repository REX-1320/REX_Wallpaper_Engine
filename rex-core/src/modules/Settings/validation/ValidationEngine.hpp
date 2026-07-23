#pragma once
#include "../models/ValidationResult.hpp"
#include "../registry/Setting.hpp"

namespace rex::settings {

class ValidationEngine {
public:
    ValidationResult validate(const Setting& setting, const SettingValue& value) const;
};

} // namespace rex::settings
