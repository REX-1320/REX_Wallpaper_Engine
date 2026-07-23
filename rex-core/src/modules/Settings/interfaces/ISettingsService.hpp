#pragma once
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include "../registry/Setting.hpp"
#include "../registry/Category.hpp"
#include "../models/ValidationResult.hpp"
#include "../schema/SchemaVersion.hpp"

namespace rex::settings {

class SettingsTransaction;

class ISettingsService {
public:
    virtual ~ISettingsService() = default;

    virtual bool registerSetting(const Setting& setting) = 0;
    virtual bool unregisterSetting(const std::string& id) = 0;
    virtual bool registerCategory(const Category& category) = 0;

    virtual std::optional<SettingValue> getValue(const std::string& id) = 0;
    virtual bool setValue(const std::string& id, const SettingValue& value) = 0;
    virtual bool resetSetting(const std::string& id) = 0;
    virtual bool resetCategory(const std::string& categoryId) = 0;

    virtual ValidationResult validate(const std::string& id, const SettingValue& value) = 0;

    virtual std::unique_ptr<SettingsTransaction> beginTransaction() = 0;
    virtual void commitTransaction(std::unique_ptr<SettingsTransaction> txn) = 0;
    virtual void rollbackTransaction(std::unique_ptr<SettingsTransaction> txn) = 0;

    virtual std::vector<Setting> getAllSettings() = 0;
    virtual std::vector<Category> getAllCategories() = 0;

    virtual bool createBackup() = 0;
    virtual bool restoreBackup(const std::string& timestamp = "") = 0;
    virtual std::string exportSettings(const std::vector<std::string>& ids = {}) = 0;
    virtual bool importSettings(const std::string& json, bool overwrite = true) = 0;
    virtual SchemaVersion currentSchemaVersion() = 0;
    virtual bool migrateTo(const SchemaVersion& target) = 0;
};

}
