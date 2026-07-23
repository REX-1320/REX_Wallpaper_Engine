#pragma once
#include "../models/ValidationResult.hpp"
#include "../registry/SettingValue.hpp"
#include <functional>
#include <string>
#include <vector>

namespace rex::settings::validators {

using ValidatorFunc = std::function<ValidationResult(const SettingValue&)>;

ValidatorFunc booleanOnly();
ValidatorFunc range(int64_t min, int64_t max);
ValidatorFunc range(double min, double max);
ValidatorFunc regex(const std::string& pattern);
ValidatorFunc fileExists();
ValidatorFunc folderExists();
ValidatorFunc oneOf(const std::vector<std::string>& allowed);

} // namespace rex::settings::validators
