#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include "../models/Theme.hpp"
#include "../models/ComponentStyle.hpp"

namespace rex::ui {

class IThemeService {
public:
    virtual ~IThemeService() = default;

    virtual void registerTheme(const Theme& theme) = 0;
    virtual void setTheme(const std::string& themeId) = 0;
    virtual Theme getCurrentTheme() const = 0;
    virtual std::vector<Theme> listThemes() const = 0;

    virtual std::string getToken(const std::string& tokenPath) const = 0;

    virtual ComponentStyleDefinition getComponentStyle(const std::string& componentType) const = 0;
    virtual std::unordered_map<std::string, std::string> resolveStyle(
        const std::string& componentType,
        const std::string& variant = "",
        const std::string& state = "") const = 0;

    virtual void registerComponent(const ComponentStyleDefinition& def) = 0;

    virtual void subscribeToThemeChanges(std::function<void()> callback) = 0;
};

}
