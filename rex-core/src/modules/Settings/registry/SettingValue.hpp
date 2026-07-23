#pragma once
#include <variant>
#include <string>
#include <vector>
#include <cstdint>
#include "SettingType.hpp"

namespace rex::settings {

using SettingValue = std::variant<
    bool,
    int64_t,
    double,
    std::string,
    std::vector<std::string>,
    std::vector<uint8_t>
>;

inline SettingType typeOf(const SettingValue& val) {
    if (std::holds_alternative<bool>(val)) return SettingType::Boolean;
    if (std::holds_alternative<int64_t>(val)) return SettingType::Integer;
    if (std::holds_alternative<double>(val)) return SettingType::Float;
    if (std::holds_alternative<std::string>(val)) return SettingType::String;
    if (std::holds_alternative<std::vector<std::string>>(val)) return SettingType::List;
    if (std::holds_alternative<std::vector<uint8_t>>(val)) return SettingType::Object;
    return SettingType::String;
}

inline bool valuesEqual(const SettingValue& a, const SettingValue& b) {
    return a == b;
}

} // namespace rex::settings
