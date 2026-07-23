#pragma once
#include "../interfaces/IThemeService.hpp"
#include "ThemeRegistry.hpp"
#include "TokenManager.hpp"
#include "StyleResolver.hpp"
#include "ComponentRegistry.hpp"
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>

namespace rex::ui {

class ThemeService : public IThemeService {
public:
    ThemeService(std::unique_ptr<ThemeRegistry> themeReg,
                 std::unique_ptr<TokenManager> tokenMgr,
                 std::unique_ptr<StyleResolver> styleResolver,
                 std::unique_ptr<ComponentRegistry> compReg);

    void registerTheme(const Theme& theme) override;
    void setTheme(const std::string& themeId) override;
    Theme getCurrentTheme() const override;
    std::vector<Theme> listThemes() const override;

    std::string getToken(const std::string& tokenPath) const override;
    ComponentStyleDefinition getComponentStyle(const std::string& componentType) const override;
    std::unordered_map<std::string, std::string> resolveStyle(
        const std::string& componentType,
        const std::string& variant = "",
        const std::string& state = "") const override;
    void registerComponent(const ComponentStyleDefinition& def) override;

    void subscribeToThemeChanges(std::function<void()> callback) override;

    void loadBuiltInThemes();

    void loadThemeTokens(const std::string& themeId,
                         const std::unordered_map<std::string, std::string>& tokens);

private:
    std::unique_ptr<ThemeRegistry> themeRegistry_;
    std::unique_ptr<TokenManager> tokenManager_;
    std::unique_ptr<StyleResolver> styleResolver_;
    std::unique_ptr<ComponentRegistry> componentRegistry_;

    std::string currentThemeId_;
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> themeChangeCallbacks_;

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> themeTokens_;
};

}
