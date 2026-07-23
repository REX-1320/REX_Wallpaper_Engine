#pragma once
#include <unordered_map>
#include <mutex>
#include <optional>
#include "Setting.hpp"
#include "Category.hpp"

namespace rex::settings {
class SettingsRegistry {
public:
    bool addSetting(const Setting& setting);
    bool removeSetting(const std::string& id);
    const Setting* getSetting(const std::string& id) const;
    std::vector<Setting> getAllSettings() const;
    std::vector<Setting> getSettingsByCategory(const std::string& category) const;

    bool setValue(const std::string& id, const SettingValue& value);
    std::optional<SettingValue> getValue(const std::string& id) const;

    bool addCategory(const Category& category);
    const Category* getCategory(const std::string& id) const;
    std::vector<Category> getAllCategories() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Setting> settings_;
    std::unordered_map<std::string, Category> categories_;
    std::unordered_map<std::string, std::vector<std::string>> categoryIndex_;
};
}
