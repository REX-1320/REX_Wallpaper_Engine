#include "ValidationEngine.hpp"

namespace rex::settings {

ValidationResult ValidationEngine::validate(const Setting& setting, const SettingValue& value) const {
    for (const auto& val : setting.validators) {
        auto res = val(value);
        if (!res.valid) return res;
    }
    return {true};
}

} // namespace rex::settings
