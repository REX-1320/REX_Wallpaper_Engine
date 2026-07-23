#pragma once
#include <string>
#include <vector>
#include <sstream>
#include "../registry/SettingsRegistry.hpp"
#include "../validation/ValidationEngine.hpp"

namespace rex::settings {

class ISettingsService;

class ImportExportManager {
public:
    ImportExportManager(SettingsRegistry& registry, ValidationEngine& validator);

    std::string exportToJson(const std::vector<std::string>& settingIds = {}) const;
    bool importFromJson(const std::string& json, bool overwrite, ISettingsService& service);

private:
    static std::string serializeValue(const SettingValue& val);
    static bool deserializeValue(const std::string& str, SettingType type, SettingValue& out);

    SettingsRegistry& registry_;
    ValidationEngine& validator_;
};

}
