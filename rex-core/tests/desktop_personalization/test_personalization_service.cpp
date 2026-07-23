#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "desktop_personalization/models/Theme.hpp"
#include "desktop_personalization/models/AccentColor.hpp"
#include "desktop_personalization/models/VisualEffects.hpp"
#include "desktop_personalization/models/DesktopSettings.hpp"
#include "desktop_personalization/models/Profile.hpp"
#include "desktop_personalization/platform/IPlatformAdapter.hpp"
#include "desktop_personalization/services/ThemeManager.hpp"
#include "desktop_personalization/services/EffectsManager.hpp"
#include "desktop_personalization/services/DesktopManager.hpp"
#include "desktop_personalization/services/PersonalizationService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/ServiceContainer.hpp"

using namespace rex::desktop_personalization;
using namespace rex::desktop_personalization::platform;

namespace {

class MockPlatformAdapter : public IPlatformAdapter {
public:
    bool darkModeSet = false;
    AccentColor lastAccent;
    bool transparencySet = false;
    bool blurSet = false;
    bool acrylicSet = false;
    bool micaSet = false;
    bool animationsSet = false;
    int lastAnimationSpeed = 200;
    int iconSizeSet = 32;
    int iconHSpacingSet = 43;
    int iconVSpacingSet = 43;
    bool labelVisibilitySet = true;

    bool setSystemDarkMode(bool dark) override { darkModeSet = dark; return true; }
    bool setAccentColor(const AccentColor& color) override { lastAccent = color; return true; }
    std::optional<AccentColor> getCurrentAccentColor() override { return AccentColor::defaultAccent(); }

    bool setTransparency(bool enable) override { transparencySet = enable; return true; }
    bool setBlur(bool enable) override { blurSet = enable; return true; }
    bool setAcrylic(bool enable) override { acrylicSet = enable; return true; }
    bool setMica(bool enable) override { micaSet = enable; return true; }
    bool setAnimations(bool enable) override { animationsSet = enable; return true; }
    bool setAnimationSpeed(int ms) override { lastAnimationSpeed = ms; return true; }

    bool setDesktopIconSize(int size) override { iconSizeSet = size; return true; }
    bool setDesktopIconSpacing(int horiz, int vert) override { iconHSpacingSet = horiz; iconVSpacingSet = vert; return true; }
    bool setDesktopLabelsVisible(bool visible) override { labelVisibilitySet = visible; return true; }
    bool refreshDesktop() override { return true; }

    bool restartExplorer() override { return true; }
    bool setTaskbarAutoHide(bool) override { return true; }
    bool setTaskbarAlignment(int) override { return true; }
    bool applyAllChanges() override { return true; }
};

std::shared_ptr<PersonalizationService> createTestService(
    std::shared_ptr<MockPlatformAdapter> platform,
    std::shared_ptr<rex::modules::EventBus> bus)
{
    auto config = std::make_shared<rex::modules::ConfigurationStore>("test_personalization_config.json");
    rex::modules::ServiceContainer container;
    container.registerService(bus);
    container.registerService(config);

    auto themeMgr  = std::make_shared<ThemeManager>(platform, bus);
    auto appearMgr = std::make_shared<AppearanceManager>(platform, bus);
    auto effMgr    = std::make_shared<EffectsManager>(platform, bus);
    auto deskMgr   = std::make_shared<DesktopManager>(platform, bus, container);
    auto profMgr   = std::make_shared<ProfileManager>(config, bus);
    auto setMgr    = std::make_shared<SettingsManager>(config, bus);

    return std::make_shared<PersonalizationService>(platform, themeMgr, appearMgr, effMgr, deskMgr, profMgr, setMgr);
}

} // namespace

TEST(PersonalizationServiceTest, ApplyThemeSetsDarkMode) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    Theme darkTheme;
    darkTheme.darkMode = true;
    EXPECT_TRUE(service->applyTheme(darkTheme));
    EXPECT_TRUE(platform->darkModeSet);
}

TEST(PersonalizationServiceTest, ApplyThemeSetsAccentColor) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    Theme theme;
    theme.accentColor = AccentColor{255, 0, 0, 255};
    EXPECT_TRUE(service->applyTheme(theme));
    EXPECT_EQ(platform->lastAccent.r, 255);
    EXPECT_EQ(platform->lastAccent.g, 0);
    EXPECT_EQ(platform->lastAccent.b, 0);
}

TEST(PersonalizationServiceTest, ApplyEffectsSetsTransparency) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    VisualEffects fx;
    fx.transparency = true;
    fx.blur = false;
    fx.mica = true;
    EXPECT_TRUE(service->applyEffects(fx));
    EXPECT_TRUE(platform->transparencySet);
    EXPECT_FALSE(platform->blurSet);
    EXPECT_TRUE(platform->micaSet);
}

TEST(PersonalizationServiceTest, ApplyEffectsSetsAnimationSpeed) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    VisualEffects fx;
    fx.animationSpeedMs = 500;
    EXPECT_TRUE(service->applyEffects(fx));
    EXPECT_EQ(platform->lastAnimationSpeed, 500);
}

TEST(PersonalizationServiceTest, ApplyDesktopSettings) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    DesktopSettings ds;
    ds.iconSize = 48;
    ds.iconSpacingHorizontal = 60;
    ds.showLabels = false;
    EXPECT_TRUE(service->applyDesktopSettings(ds));
    EXPECT_EQ(platform->iconSizeSet, 48);
    EXPECT_EQ(platform->iconHSpacingSet, 60);
    EXPECT_FALSE(platform->labelVisibilitySet);
}

TEST(PersonalizationServiceTest, PreviewThemeDoesNotPersist) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    Theme original;
    original.darkMode = false;
    service->applyTheme(original);
    EXPECT_FALSE(platform->darkModeSet);

    Theme preview;
    preview.darkMode = true;
    EXPECT_TRUE(service->previewTheme(preview));
    EXPECT_TRUE(platform->darkModeSet);
}

TEST(PersonalizationServiceTest, RollbackRestoresPreviousState) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    Theme light;
    light.darkMode = false;
    service->applyTheme(light);

    Theme dark;
    dark.darkMode = true;
    service->applyTheme(dark);
    EXPECT_TRUE(platform->darkModeSet);

    service->rollbackToLastGood();
    EXPECT_FALSE(platform->darkModeSet);
}

TEST(PersonalizationServiceTest, ResetToDefaults) {
    auto platform = std::make_shared<MockPlatformAdapter>();
    auto bus = std::make_shared<rex::modules::EventBus>();
    auto service = createTestService(platform, bus);

    EXPECT_TRUE(service->resetToDefaults());
}

TEST(PersonalizationTest, AccentColorDefaultAccent) {
    AccentColor c = AccentColor::defaultAccent();
    EXPECT_EQ(c.r, 0);
    EXPECT_EQ(c.g, 120);
    EXPECT_EQ(c.b, 215);
    EXPECT_EQ(c.a, 255);
}

TEST(PersonalizationTest, ThemeDefaultValues) {
    Theme t;
    EXPECT_FALSE(t.darkMode);
    EXPECT_TRUE(t.transparency);
    EXPECT_TRUE(t.animations);
}

TEST(PersonalizationTest, VisualEffectsDefaultValues) {
    VisualEffects fx;
    EXPECT_TRUE(fx.transparency);
    EXPECT_TRUE(fx.blur);
    EXPECT_FALSE(fx.mica);
    EXPECT_FALSE(fx.performanceMode);
}

TEST(PersonalizationTest, DesktopSettingsDefaultValues) {
    DesktopSettings ds;
    EXPECT_EQ(ds.iconSize, 32);
    EXPECT_TRUE(ds.showLabels);
    EXPECT_TRUE(ds.gridEnabled);
}

TEST(PersonalizationTest, ProfileCreation) {
    Profile p;
    p.name = "test_profile";
    p.description = "A test profile";
    p.theme.darkMode = true;
    p.version = 1;
    EXPECT_EQ(p.name, "test_profile");
    EXPECT_TRUE(p.theme.darkMode);
}

TEST(EventBusTest, PersonalizationEvents) {
    auto bus = std::make_shared<rex::modules::EventBus>();
    rex::modules::ModuleEvent received;
    bool receivedEvent = false;

    bus->subscribe([&](const rex::modules::ModuleEvent& ev) {
        received = ev;
        receivedEvent = true;
    });

    bus->publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::ThemeChanged,
        "rex.desktop_personalization",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });

    EXPECT_TRUE(receivedEvent);
    EXPECT_EQ(received.type, rex::modules::ModuleEventType::ThemeChanged);
    EXPECT_EQ(received.moduleId, "rex.desktop_personalization");
}
