#include "ThemeRegistry.hpp"

namespace rex::ui {

void ThemeRegistry::registerTheme(const Theme& theme) {
    std::lock_guard lock(mutex_);
    themes_[theme.id] = theme;
}

const Theme* ThemeRegistry::getTheme(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = themes_.find(id);
    return it != themes_.end() ? &it->second : nullptr;
}

std::vector<Theme> ThemeRegistry::getAllThemes() const {
    std::lock_guard lock(mutex_);
    std::vector<Theme> result;
    for (auto& [id, theme] : themes_) result.push_back(theme);
    return result;
}

}
