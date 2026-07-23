#pragma once
#include <string>
#include <optional>
#include <vector>
#include "../registry/SettingValue.hpp"

namespace rex::settings {

class SettingsHistory {
public:
    struct Change {
        std::string settingId;
        SettingValue oldValue;
        SettingValue newValue;
    };

    void record(const std::string& id, const SettingValue& oldVal, const SettingValue& newVal);
    std::optional<Change> undo();
    std::optional<Change> redo();
    void clear();

private:
    std::vector<Change> past_;
    std::vector<Change> future_;
};

}
