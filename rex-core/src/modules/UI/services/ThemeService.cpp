#include "ThemeService.hpp"
#include <stdexcept>
#include <vector>

namespace rex::ui {

ThemeService::ThemeService(std::unique_ptr<ThemeRegistry> themeReg,
                           std::unique_ptr<TokenManager> tokenMgr,
                           std::unique_ptr<StyleResolver> styleResolver,
                           std::unique_ptr<ComponentRegistry> compReg)
    : themeRegistry_(std::move(themeReg))
    , tokenManager_(std::move(tokenMgr))
    , styleResolver_(std::move(styleResolver))
    , componentRegistry_(std::move(compReg)) {}

void ThemeService::registerTheme(const Theme& theme) {
    themeRegistry_->registerTheme(theme);
}

void ThemeService::loadThemeTokens(const std::string& themeId,
                                   const std::unordered_map<std::string, std::string>& tokens) {
    themeTokens_[themeId] = tokens;
}

void ThemeService::setTheme(const std::string& themeId) {
    const Theme* theme = themeRegistry_->getTheme(themeId);
    if (!theme) throw std::runtime_error("Theme not found: " + themeId);

    std::unordered_map<std::string, std::string> accumulated;

    std::vector<const Theme*> chain;
    const Theme* cur = theme;
    while (cur) {
        chain.push_back(cur);
        if (cur->parentThemeId.empty()) break;
        cur = themeRegistry_->getTheme(cur->parentThemeId);
        if (!cur) break;
    }

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        auto tokenIt = themeTokens_.find((*it)->id);
        if (tokenIt != themeTokens_.end()) {
            for (auto& [path, value] : tokenIt->second) {
                accumulated[path] = value;
            }
        }
    }

    tokenManager_->loadThemeTokens(*theme, accumulated, nullptr);

    {
        std::lock_guard lock(mutex_);
        currentThemeId_ = themeId;
    }

    for (auto& cb : themeChangeCallbacks_) {
        if (cb) cb();
    }
}

Theme ThemeService::getCurrentTheme() const {
    std::lock_guard lock(mutex_);
    const Theme* t = themeRegistry_->getTheme(currentThemeId_);
    if (t) return *t;
    return Theme{};
}

std::vector<Theme> ThemeService::listThemes() const {
    return themeRegistry_->getAllThemes();
}

std::string ThemeService::getToken(const std::string& tokenPath) const {
    return tokenManager_->getToken(tokenPath);
}

ComponentStyleDefinition ThemeService::getComponentStyle(const std::string& componentType) const {
    const ComponentStyleDefinition* def = componentRegistry_->getDefinition(componentType);
    if (def) return *def;
    return ComponentStyleDefinition{};
}

std::unordered_map<std::string, std::string> ThemeService::resolveStyle(
    const std::string& componentType,
    const std::string& variant,
    const std::string& state) const {
    const ComponentStyleDefinition* def = componentRegistry_->getDefinition(componentType);
    if (!def) return {};
    return styleResolver_->resolve(*def, variant, state);
}

void ThemeService::registerComponent(const ComponentStyleDefinition& def) {
    componentRegistry_->registerComponent(def);
}

void ThemeService::subscribeToThemeChanges(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    themeChangeCallbacks_.push_back(std::move(callback));
}

void ThemeService::loadBuiltInThemes() {
    Theme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.dark = true;
    registerTheme(dark);

    Theme light;
    light.id = "light";
    light.name = "Light";
    light.dark = false;
    registerTheme(light);

    Theme amoled;
    amoled.id = "amoled";
    amoled.name = "AMOLED";
    amoled.parentThemeId = "dark";
    amoled.dark = true;
    registerTheme(amoled);

    loadThemeTokens("dark", {
        {"Color.Primary", "#4488FF"},
        {"Color.Secondary", "#FF4488"},
        {"Color.Background", "#1E1E1E"},
        {"Color.Surface", "#2D2D2D"},
        {"Color.Error", "#FF4444"},
        {"Text.Primary", "#FFFFFF"},
        {"Text.OnPrimary", "#FFFFFF"},
        {"Text.OnError", "#FFFFFF"},
        {"Text.Primary.Hover", "#CCCCCC"},
        {"Text.OnPrimary.Hover", "#E0E0E0"},
        {"Surface.Background", "#2D2D2D"},
        {"Radius.Medium", "8px"},
        {"Radius.Medium.Hover", "10px"},
        {"Spacing.Medium", "12px"},
        {"Elevation.Level1", "2px"},
        {"Color.Primary.Hover", "#3377EE"},
        {"Color.Error.Hover", "#CC3333"}
    });

    loadThemeTokens("light", {
        {"Color.Primary", "#4488FF"},
        {"Color.Secondary", "#FF4488"},
        {"Color.Background", "#FFFFFF"},
        {"Color.Surface", "#F5F5F5"},
        {"Color.Error", "#FF4444"},
        {"Text.Primary", "#000000"},
        {"Text.OnPrimary", "#FFFFFF"},
        {"Text.OnError", "#FFFFFF"},
        {"Text.Primary.Hover", "#333333"},
        {"Text.OnPrimary.Hover", "#E0E0E0"},
        {"Surface.Background", "#F5F5F5"},
        {"Radius.Medium", "8px"},
        {"Radius.Medium.Hover", "10px"},
        {"Spacing.Medium", "12px"},
        {"Elevation.Level1", "2px"},
        {"Color.Primary.Hover", "#3377EE"},
        {"Color.Error.Hover", "#CC3333"}
    });

    loadThemeTokens("amoled", {
        {"Color.Background", "#000000"},
        {"Color.Surface", "#0A0A0A"},
        {"Surface.Background", "#0A0A0A"},
        {"Radius.Medium.Hover", "12px"}
    });

    setTheme("dark");
}

}
