#include "SettingsRegistry.hpp"

namespace rex::settings {

bool SettingsRegistry::addSetting(const Setting& setting) {
    std::lock_guard lock(mutex_);
    if (settings_.count(setting.id)) return false;
    auto s = setting;
    s.currentValue = s.defaultValue;
    settings_[setting.id] = s;
    categoryIndex_[setting.category].push_back(setting.id);
    return true;
}

bool SettingsRegistry::removeSetting(const std::string& id) {
    std::lock_guard lock(mutex_);
    return settings_.erase(id) > 0;
}

const Setting* SettingsRegistry::getSetting(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = settings_.find(id);
    return it != settings_.end() ? &it->second : nullptr;
}

std::vector<Setting> SettingsRegistry::getAllSettings() const {
    std::lock_guard lock(mutex_);
    std::vector<Setting> result;
    for (const auto& [id, s] : settings_) result.push_back(s);
    return result;
}

std::vector<Setting> SettingsRegistry::getSettingsByCategory(const std::string& category) const {
    std::lock_guard lock(mutex_);
    std::vector<Setting> res;
    auto it = categoryIndex_.find(category);
    if (it != categoryIndex_.end()) {
        for (const auto& id : it->second) {
            auto sit = settings_.find(id);
            if (sit != settings_.end()) res.push_back(sit->second);
        }
    }
    return res;
}

bool SettingsRegistry::setValue(const std::string& id, const SettingValue& value) {
    std::lock_guard lock(mutex_);
    auto it = settings_.find(id);
    if (it == settings_.end()) return false;
    it->second.currentValue = value;
    return true;
}

std::optional<SettingValue> SettingsRegistry::getValue(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = settings_.find(id);
    if (it == settings_.end()) return std::nullopt;
    return it->second.currentValue;
}

bool SettingsRegistry::addCategory(const Category& category) {
    std::lock_guard lock(mutex_);
    return categories_.emplace(category.id, category).second;
}

const Category* SettingsRegistry::getCategory(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = categories_.find(id);
    return it != categories_.end() ? &it->second : nullptr;
}

std::vector<Category> SettingsRegistry::getAllCategories() const {
    std::lock_guard lock(mutex_);
    std::vector<Category> result;
    for (const auto& [id, cat] : categories_) result.push_back(cat);
    return result;
}

} // namespace
