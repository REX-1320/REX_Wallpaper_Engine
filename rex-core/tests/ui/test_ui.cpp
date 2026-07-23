#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "modules/UI/models/Theme.hpp"
#include "modules/UI/models/Token.hpp"
#include "modules/UI/models/ComponentStyle.hpp"
#include "modules/UI/interfaces/IThemeService.hpp"
#include "modules/UI/interfaces/IThemeProvider.hpp"
#include "modules/UI/services/ThemeRegistry.hpp"
#include "modules/UI/services/TokenManager.hpp"
#include "modules/UI/services/ComponentRegistry.hpp"
#include "modules/UI/services/StyleResolver.hpp"
#include "modules/UI/services/ThemeService.hpp"

namespace rex::ui {

// ─── Theme model ───────────────────────────────────────────────────────────

TEST(UIModelTest, ThemeDefaults) {
    Theme t;
    EXPECT_TRUE(t.id.empty());
    EXPECT_TRUE(t.name.empty());
    EXPECT_TRUE(t.parentThemeId.empty());
    EXPECT_FALSE(t.dark);
}

TEST(UIModelTest, ThemeDark) {
    Theme t;
    t.id = "dark";
    t.name = "Dark";
    t.dark = true;
    EXPECT_EQ(t.id, "dark");
    EXPECT_EQ(t.name, "Dark");
    EXPECT_TRUE(t.dark);
}

TEST(UIModelTest, ThemeWithParent) {
    Theme t;
    t.id = "amoled";
    t.parentThemeId = "dark";
    EXPECT_EQ(t.parentThemeId, "dark");
}

// ─── TokenCategory ─────────────────────────────────────────────────────────

TEST(UIModelTest, TokenCategoryValues) {
    EXPECT_NE(static_cast<int>(TokenCategory::Color),
              static_cast<int>(TokenCategory::Typography));
    EXPECT_EQ(static_cast<int>(TokenCategory::Radius) + 1,
              static_cast<int>(TokenCategory::Shadow));
}

// ─── ComponentStyleDefinition ──────────────────────────────────────────────

TEST(UIModelTest, ComponentStyleDefaults) {
    ComponentStyleDefinition def;
    EXPECT_TRUE(def.componentType.empty());
    EXPECT_TRUE(def.defaultProperties.empty());
    EXPECT_TRUE(def.variants.empty());
}

TEST(UIModelTest, ComponentStyleWithVariant) {
    ComponentStyleDefinition def;
    def.componentType = "Button";
    def.defaultProperties["background"] = "Color.Primary";
    def.defaultProperties["foreground"] = "Text.Primary";

    ComponentStyleDefinition::Variant v;
    v.name = "Primary";
    v.properties["background"] = "Color.Primary";
    v.properties["foreground"] = "Text.OnPrimary";
    def.variants.push_back(v);

    EXPECT_EQ(def.componentType, "Button");
    EXPECT_EQ(def.defaultProperties.size(), 2u);
    EXPECT_EQ(def.variants.size(), 1u);
    EXPECT_EQ(def.variants[0].name, "Primary");
}

// ─── ThemeRegistry ─────────────────────────────────────────────────────────

class ThemeRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry = std::make_unique<ThemeRegistry>();
    }

    std::unique_ptr<ThemeRegistry> registry;
};

TEST_F(ThemeRegistryTest, RegisterAndGet) {
    Theme t;
    t.id = "dark";
    t.name = "Dark";
    t.dark = true;
    registry->registerTheme(t);

    const Theme* retrieved = registry->getTheme("dark");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->id, "dark");
    EXPECT_EQ(retrieved->name, "Dark");
    EXPECT_TRUE(retrieved->dark);
}

TEST_F(ThemeRegistryTest, GetNonexistentReturnsNull) {
    const Theme* retrieved = registry->getTheme("nonexistent");
    EXPECT_EQ(retrieved, nullptr);
}

TEST_F(ThemeRegistryTest, GetAllThemes) {
    Theme dark;
    dark.id = "dark";
    registry->registerTheme(dark);

    Theme light;
    light.id = "light";
    registry->registerTheme(light);

    auto all = registry->getAllThemes();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(ThemeRegistryTest, RegisterOverwrites) {
    Theme t1;
    t1.id = "dark";
    t1.name = "Dark";
    registry->registerTheme(t1);

    Theme t2;
    t2.id = "dark";
    t2.name = "Dark V2";
    registry->registerTheme(t2);

    const Theme* retrieved = registry->getTheme("dark");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->name, "Dark V2");
}

// ─── TokenManager ──────────────────────────────────────────────────────────

class TokenManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr = std::make_unique<TokenManager>();
    }

    std::unique_ptr<TokenManager> mgr;
};

TEST_F(TokenManagerTest, GetTokenEmptyTheme) {
    Theme t;
    t.id = "test";
    mgr->loadThemeTokens(t, {});
    EXPECT_EQ(mgr->getToken("Color.Primary"), "");
}

TEST_F(TokenManagerTest, LoadAndGet) {
    Theme t;
    t.id = "test";
    mgr->loadThemeTokens(t, {
        {"Color.Primary", "#4488FF"},
        {"Color.Background", "#1E1E1E"}
    });
    EXPECT_EQ(mgr->getToken("Color.Primary"), "#4488FF");
    EXPECT_EQ(mgr->getToken("Color.Background"), "#1E1E1E");
    EXPECT_EQ(mgr->getToken("Color.Nonexistent"), "");
}

TEST_F(TokenManagerTest, OverrideToken) {
    Theme t;
    t.id = "test";
    mgr->loadThemeTokens(t, {{"Color.Primary", "#4488FF"}});
    EXPECT_EQ(mgr->getToken("Color.Primary"), "#4488FF");

    mgr->setTokenOverride("Color.Primary", "#FF8800");
    EXPECT_EQ(mgr->getToken("Color.Primary"), "#FF8800");
}

TEST_F(TokenManagerTest, LoadWithParentInheritance) {
    Theme parent;
    parent.id = "parent";
    Theme child;
    child.id = "child";

    TokenManager parentMgr;
    parentMgr.loadThemeTokens(parent, {
        {"Color.Primary", "#4488FF"},
        {"Color.Background", "#1E1E1E"}
    });

    mgr->loadThemeTokens(child, {
        {"Color.Primary", "#FF4488"}
    }, &parentMgr);

    EXPECT_EQ(mgr->getToken("Color.Primary"), "#FF4488");
    EXPECT_EQ(mgr->getToken("Color.Background"), "#1E1E1E");
}

// ─── ComponentRegistry ─────────────────────────────────────────────────────

class ComponentRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        reg = std::make_unique<ComponentRegistry>();
    }

    std::unique_ptr<ComponentRegistry> reg;
};

TEST_F(ComponentRegistryTest, RegisterAndGet) {
    ComponentStyleDefinition def;
    def.componentType = "Button";
    def.defaultProperties["background"] = "Color.Primary";
    reg->registerComponent(def);

    const ComponentStyleDefinition* retrieved = reg->getDefinition("Button");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->componentType, "Button");
}

TEST_F(ComponentRegistryTest, GetNonexistentReturnsNull) {
    EXPECT_EQ(reg->getDefinition("Nonexistent"), nullptr);
}

// ─── StyleResolver ─────────────────────────────────────────────────────────

class StyleResolverTest : public ::testing::Test {
protected:
    void SetUp() override {
        tokenMgr = std::make_unique<TokenManager>();
        Theme t;
        t.id = "test";
        tokenMgr->loadThemeTokens(t, {
            {"Color.Primary", "#4488FF"},
            {"Color.Error", "#FF4444"},
            {"Text.Primary", "#FFFFFF"},
            {"Text.OnPrimary", "#FFFFFF"},
            {"Text.OnError", "#FFFFFF"},
            {"Radius.Medium", "8px"},
            {"Spacing.Medium", "12px"},
            {"Color.Primary.Hover", "#3377EE"},
            {"Radius.Medium.Hover", "10px"}
        });
        resolver = std::make_unique<StyleResolver>(*tokenMgr);

        def.componentType = "Button";
        def.defaultProperties = {
            {"background", "Color.Primary"},
            {"foreground", "Text.Primary"},
            {"borderRadius", "Radius.Medium"},
            {"padding", "Spacing.Medium"}
        };
    }

    std::unique_ptr<TokenManager> tokenMgr;
    std::unique_ptr<StyleResolver> resolver;
    ComponentStyleDefinition def;
};

TEST_F(StyleResolverTest, ResolveDefaultNoVariant) {
    auto props = resolver->resolve(def, "", "");
    EXPECT_EQ(props["background"], "#4488FF");
    EXPECT_EQ(props["foreground"], "#FFFFFF");
    EXPECT_EQ(props["borderRadius"], "8px");
    EXPECT_EQ(props["padding"], "12px");
}

TEST_F(StyleResolverTest, ResolveWithVariant) {
    ComponentStyleDefinition::Variant v;
    v.name = "Primary";
    v.properties["background"] = "Color.Primary";
    v.properties["foreground"] = "Text.OnPrimary";
    def.variants.push_back(v);

    auto props = resolver->resolve(def, "Primary", "");
    EXPECT_EQ(props["background"], "#4488FF");
    EXPECT_EQ(props["foreground"], "#FFFFFF");
}

TEST_F(StyleResolverTest, ResolveWithState) {
    auto props = resolver->resolve(def, "", "Hover");
    EXPECT_EQ(props["background"], "#3377EE");
    EXPECT_EQ(props["borderRadius"], "10px");
}

TEST_F(StyleResolverTest, ResolveWithVariantAndState) {
    ComponentStyleDefinition::Variant v;
    v.name = "Primary";
    v.properties["background"] = "Color.Primary";
    v.properties["foreground"] = "Text.OnPrimary";
    def.variants.push_back(v);

    auto props = resolver->resolve(def, "Primary", "Hover");
    EXPECT_EQ(props["background"], "#3377EE");
    EXPECT_EQ(props["foreground"], "#FFFFFF");
}

TEST_F(StyleResolverTest, ResolveNonexistentTokenReturnsEmpty) {
    def.defaultProperties["nonexistent"] = "Path.Does.Not.Exist";
    auto props = resolver->resolve(def, "", "");
    EXPECT_EQ(props["nonexistent"], "");
}

// ─── ThemeService ──────────────────────────────────────────────────────────

class ThemeServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto themeReg = std::make_unique<ThemeRegistry>();
        auto tokenMgr = std::make_unique<TokenManager>();
        auto styleRes = std::make_unique<StyleResolver>(*tokenMgr);
        auto compReg = std::make_unique<ComponentRegistry>();

        service_ = std::make_unique<ThemeService>(
            std::move(themeReg),
            std::move(tokenMgr),
            std::move(styleRes),
            std::move(compReg));
    }

    std::unique_ptr<ThemeService> service_;
};

TEST_F(ThemeServiceTest, RegisterAndListThemes) {
    Theme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.dark = true;
    service_->registerTheme(dark);

    Theme light;
    light.id = "light";
    light.name = "Light";
    light.dark = false;
    service_->registerTheme(light);

    auto themes = service_->listThemes();
    EXPECT_EQ(themes.size(), 2u);
}

TEST_F(ThemeServiceTest, LoadBuiltInThemes) {
    service_->loadBuiltInThemes();
    auto themes = service_->listThemes();
    EXPECT_EQ(themes.size(), 3u);
}

TEST_F(ThemeServiceTest, SetTheme) {
    Theme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.dark = true;
    service_->registerTheme(dark);
    service_->loadThemeTokens("dark", {{"Color.Primary", "#4488FF"}});
    service_->setTheme("dark");

    auto current = service_->getCurrentTheme();
    EXPECT_EQ(current.id, "dark");
    EXPECT_EQ(service_->getToken("Color.Primary"), "#4488FF");
}

TEST_F(ThemeServiceTest, SetNonexistentThemeThrows) {
    EXPECT_THROW(service_->setTheme("nonexistent"), std::runtime_error);
}

TEST_F(ThemeServiceTest, GetTokenBeforeThemeSet) {
    EXPECT_EQ(service_->getToken("Color.Primary"), "");
}

TEST_F(ThemeServiceTest, RegisterAndResolveComponent) {
    ComponentStyleDefinition buttonDef;
    buttonDef.componentType = "Button";
    buttonDef.defaultProperties = {
        {"background", "Color.Primary"},
        {"foreground", "Text.Primary"}
    };
    service_->registerComponent(buttonDef);

    Theme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.dark = true;
    service_->registerTheme(dark);
    service_->loadThemeTokens("dark", {
        {"Color.Primary", "#4488FF"},
        {"Text.Primary", "#FFFFFF"}
    });
    service_->setTheme("dark");

    auto def = service_->getComponentStyle("Button");
    EXPECT_EQ(def.componentType, "Button");

    auto props = service_->resolveStyle("Button");
    EXPECT_EQ(props["background"], "#4488FF");
    EXPECT_EQ(props["foreground"], "#FFFFFF");
}

TEST_F(ThemeServiceTest, ResolveNonexistentComponentReturnsEmpty) {
    auto props = service_->resolveStyle("Nonexistent");
    EXPECT_TRUE(props.empty());
}

TEST_F(ThemeServiceTest, SubscribeToThemeChanges) {
    Theme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.dark = true;
    service_->registerTheme(dark);
    service_->loadThemeTokens("dark", {});

    int callCount = 0;
    service_->subscribeToThemeChanges([&callCount]() {
        callCount++;
    });

    service_->setTheme("dark");
    EXPECT_EQ(callCount, 1);

    service_->setTheme("dark");
    EXPECT_EQ(callCount, 2);
}

TEST_F(ThemeServiceTest, GetComponentStyleNonexistentReturnsEmpty) {
    ComponentStyleDefinition def = service_->getComponentStyle("Nonexistent");
    EXPECT_TRUE(def.componentType.empty());
}

TEST_F(ThemeServiceTest, GetCurrentThemeBeforeSetReturnsEmpty) {
    Theme current = service_->getCurrentTheme();
    EXPECT_TRUE(current.id.empty());
}

// ─── Inheritance ──────────────────────────────────────────────────────────

TEST_F(ThemeServiceTest, ThemeTokenInheritance) {
    service_->registerTheme([]() {
        Theme t; t.id = "dark"; t.name = "Dark"; t.dark = true; return t;
    }());
    service_->registerTheme([]() {
        Theme t; t.id = "amoled"; t.name = "AMOLED"; t.parentThemeId = "dark"; t.dark = true; return t;
    }());

    service_->loadThemeTokens("dark", {
        {"Color.Primary", "#4488FF"},
        {"Color.Background", "#1E1E1E"}
    });
    service_->loadThemeTokens("amoled", {
        {"Color.Background", "#000000"}
    });

    service_->setTheme("amoled");
    EXPECT_EQ(service_->getToken("Color.Primary"), "#4488FF");
    EXPECT_EQ(service_->getToken("Color.Background"), "#000000");
}

TEST_F(ThemeServiceTest, ThemeChangeCallbackCalledOnSet) {
    Theme dark;
    dark.id = "dark";
    dark.name = "Dark";
    dark.dark = true;
    service_->registerTheme(dark);
    service_->loadThemeTokens("dark", {});

    int count = 0;
    service_->subscribeToThemeChanges([&count]() { ++count; });
    service_->setTheme("dark");
    EXPECT_EQ(count, 1);
}

// ─── Built-in integration ──────────────────────────────────────────────────

TEST_F(ThemeServiceTest, BuiltInThemesHaveTokens) {
    service_->loadBuiltInThemes();
    EXPECT_EQ(service_->listThemes().size(), 3u);

    EXPECT_EQ(service_->getToken("Color.Primary"), "#4488FF");
    EXPECT_EQ(service_->getToken("Color.Background"), "#1E1E1E");
    EXPECT_EQ(service_->getToken("Text.Primary"), "#FFFFFF");
    EXPECT_EQ(service_->getToken("Radius.Medium"), "8px");
    EXPECT_EQ(service_->getToken("Spacing.Medium"), "12px");
}

TEST_F(ThemeServiceTest, BuiltInSwitchToLight) {
    service_->loadBuiltInThemes();
    service_->setTheme("light");
    EXPECT_EQ(service_->getToken("Color.Background"), "#FFFFFF");
    EXPECT_EQ(service_->getToken("Text.Primary"), "#000000");
}

TEST_F(ThemeServiceTest, BuiltInSwitchToAmoled) {
    service_->loadBuiltInThemes();
    service_->setTheme("amoled");

    EXPECT_EQ(service_->getToken("Color.Background"), "#000000");
    EXPECT_EQ(service_->getToken("Color.Surface"), "#0A0A0A");

    EXPECT_EQ(service_->getToken("Color.Primary"), "#4488FF");
}

// ─── IThemeProvider mock ──────────────────────────────────────────────────

class MockThemeProvider : public IThemeProvider {
public:
    void registerThemes(IThemeService& service) override {
        registered = true;
        Theme t;
        t.id = "mock";
        t.name = "Mock Theme";
        service.registerTheme(t);
    }
    bool registered = false;
};

TEST(ThemeProviderTest, MockProviderRegistersTheme) {
    auto themeReg = std::make_unique<ThemeRegistry>();
    auto tokenMgr = std::make_unique<TokenManager>();
    auto styleRes = std::make_unique<StyleResolver>(*tokenMgr);
    auto compReg = std::make_unique<ComponentRegistry>();
    auto service = std::make_unique<ThemeService>(
        std::move(themeReg), std::move(tokenMgr), std::move(styleRes), std::move(compReg));

    MockThemeProvider provider;
    provider.registerThemes(*service);
    EXPECT_TRUE(provider.registered);

    auto themes = service->listThemes();
    bool found = false;
    for (auto& t : themes) {
        if (t.id == "mock") found = true;
    }
    EXPECT_TRUE(found);
}

}
