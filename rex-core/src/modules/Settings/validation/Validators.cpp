#include "Validators.hpp"
#include <regex>
#include <filesystem>

namespace rex::settings::validators {

ValidatorFunc booleanOnly() {
    return [](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<bool>(val))
            return {false, "Expected boolean value"};
        return {true};
    };
}

ValidatorFunc range(int64_t min, int64_t max) {
    return [min, max](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<int64_t>(val))
            return {false, "Expected integer value"};
        auto v = std::get<int64_t>(val);
        if (v < min || v > max)
            return {false, "Value out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]"};
        return {true};
    };
}

ValidatorFunc range(double min, double max) {
    return [min, max](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<double>(val))
            return {false, "Expected float value"};
        auto v = std::get<double>(val);
        if (v < min || v > max)
            return {false, "Value out of range"};
        return {true};
    };
}

ValidatorFunc regex(const std::string& pattern) {
    return [pattern](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<std::string>(val))
            return {false, "Expected string value"};
        try {
            std::regex re(pattern);
            if (!std::regex_match(std::get<std::string>(val), re))
                return {false, "Value does not match pattern"};
        } catch (const std::regex_error&) {
            return {false, "Invalid regex pattern"};
        }
        return {true};
    };
}

ValidatorFunc fileExists() {
    return [](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<std::string>(val))
            return {false, "Expected path string"};
        if (!std::filesystem::exists(std::get<std::string>(val)))
            return {false, "File does not exist"};
        return {true};
    };
}

ValidatorFunc folderExists() {
    return [](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<std::string>(val))
            return {false, "Expected path string"};
        if (!std::filesystem::is_directory(std::get<std::string>(val)))
            return {false, "Directory does not exist"};
        return {true};
    };
}

ValidatorFunc oneOf(const std::vector<std::string>& allowed) {
    return [allowed](const SettingValue& val) -> ValidationResult {
        if (!std::holds_alternative<std::string>(val))
            return {false, "Expected string value"};
        auto v = std::get<std::string>(val);
        for (const auto& a : allowed) {
            if (a == v) return {true};
        }
        return {false, "Value not in allowed set"};
    };
}

} // namespace rex::settings::validators
