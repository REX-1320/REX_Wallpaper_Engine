#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../models/Theme.hpp"

namespace rex::ui {

class ThemeRegistry {
public:
    void registerTheme(const Theme& theme);

    const Theme* getTheme(const std::string& id) const;
    std::vector<Theme> getAllThemes() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Theme> themes_;
};

}
