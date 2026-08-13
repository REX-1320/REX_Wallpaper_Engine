#include "test_settings_fixtures.hpp"
#include "modules/Settings/ui/UIModelGenerator.hpp"
#include "modules/Settings/ui/UIControl.hpp"

using namespace rex::settings;

TEST(UIModelGeneratorTest, BooleanMapsToToggle) {
    Setting s;
    s.id = "test.flag"; s.name = "Flag"; s.type = SettingType::Boolean;
    s.defaultValue = true; s.visible = true; s.order = 1;
    auto controls = UIModelGenerator::generate({s});
    ASSERT_EQ(controls.size(), 1);
    EXPECT_EQ(controls[0].controlType, ui::ControlType::Toggle);
    EXPECT_EQ(controls[0].settingId, "test.flag");
}

TEST(UIModelGeneratorTest, IntegerMapsToSlider) {
    Setting s;
    s.id = "test.num"; s.name = "Num"; s.type = SettingType::Integer;
    s.defaultValue = int64_t(50); s.visible = true;
    auto controls = UIModelGenerator::generate({s});
    ASSERT_EQ(controls.size(), 1);
    EXPECT_EQ(controls[0].controlType, ui::ControlType::Slider);
}

TEST(UIModelGeneratorTest, HiddenSettingsExcluded) {
    Setting visible;   visible.id = "a"; visible.type = SettingType::Boolean;
    visible.defaultValue = false; visible.visible = true;
    Setting hidden;    hidden.id = "b"; hidden.type = SettingType::String;
    hidden.defaultValue = std::string("x"); hidden.visible = false;
    auto controls = UIModelGenerator::generate({visible, hidden});
    ASSERT_EQ(controls.size(), 1);
    EXPECT_EQ(controls[0].settingId, "a");
}

TEST(UIModelGeneratorTest, SortedByOrder) {
    Setting a; a.id = "a"; a.type = SettingType::Boolean;
    a.defaultValue = false; a.visible = true; a.order = 2;
    Setting b; b.id = "b"; b.type = SettingType::Boolean;
    b.defaultValue = false; b.visible = true; b.order = 1;
    auto controls = UIModelGenerator::generate({a, b});
    ASSERT_EQ(controls.size(), 2);
    EXPECT_EQ(controls[0].settingId, "b");
    EXPECT_EQ(controls[1].settingId, "a");
}

TEST_F(SettingsServiceTest, GetUIControlsReturnsAll) {
    Setting a; a.id = "ui.a"; a.name = "A"; a.category = "general";
    a.type = SettingType::Boolean; a.defaultValue = false; a.visible = true;
    service->registerSetting(a);
    Setting b; b.id = "ui.b"; b.name = "B"; b.category = "advanced";
    b.type = SettingType::Integer; b.defaultValue = int64_t(10); b.visible = true;
    service->registerSetting(b);

    auto all = service->getUIControls();
    ASSERT_EQ(all.size(), 2);
}

TEST_F(SettingsServiceTest, GetUIControlsFiltersByCategory) {
    Setting a; a.id = "filter.a"; a.name = "A"; a.category = "general";
    a.type = SettingType::Boolean; a.defaultValue = false; a.visible = true;
    service->registerSetting(a);
    Setting b; b.id = "filter.b"; b.name = "B"; b.category = "advanced";
    b.type = SettingType::Integer; b.defaultValue = int64_t(10); b.visible = true;
    service->registerSetting(b);

    auto general = service->getUIControls("general");
    ASSERT_EQ(general.size(), 1);
    EXPECT_EQ(general[0].settingId, "filter.a");
}

TEST(UIModelGeneratorTest, CarriesUiMetadata) {
    Setting s;
    s.id = "meta.test"; s.name = "Meta"; s.description = "desc";
    s.type = SettingType::Boolean; s.defaultValue = false;
    s.visible = true; s.readOnly = true; s.requiresRestart = true;
    s.requiresAdmin = false; s.tooltip = "hover info"; s.icon = "gear";
    s.group = "Advanced"; s.order = 5;
    auto controls = UIModelGenerator::generate({s});
    ASSERT_EQ(controls.size(), 1);
    EXPECT_TRUE(controls[0].readOnly);
    EXPECT_TRUE(controls[0].requiresRestart);
    EXPECT_FALSE(controls[0].requiresAdmin);
    EXPECT_EQ(controls[0].tooltip, "hover info");
    EXPECT_EQ(controls[0].icon, "gear");
    EXPECT_EQ(controls[0].group, "Advanced");
    EXPECT_EQ(controls[0].order, 5);
}

TEST(SettingsPersistenceTest, RoundTripAcrossServiceInstances) {
    const std::string path = "test_settings_persistence.json";
    std::remove(path.c_str());

    // The service stores references to the bus/store, so keep them in a
    // holder that outlives the service (same pattern as the runtime CLI).
    struct ServiceHolder {
        rex::modules::EventBus bus;
        std::shared_ptr<rex::modules::ConfigurationStore> store;
        std::unique_ptr<SettingsService> service;
    };

    auto makeService = [&path]() {
        auto h = std::make_shared<ServiceHolder>();
        h->store = std::make_shared<rex::modules::ConfigurationStore>(path);
        auto reg = std::make_unique<SettingsRegistry>();
        auto val = std::make_unique<ValidationEngine>();
        auto srch = std::make_unique<SettingsSearch>(*reg);
        auto hist = std::make_unique<SettingsHistory>();
        auto backup = std::make_unique<BackupManager>(h->store);
        auto schema = std::make_unique<SchemaManager>();
        auto io = std::make_unique<ImportExportManager>(*reg, *val);
        h->service = std::make_unique<SettingsService>(
            std::move(reg), std::move(val), std::move(srch), std::move(hist),
            std::move(backup), std::move(schema), std::move(io),
            h->bus, *h->store);
        h->service->hydrateSettings();
        return h;
    };

    {
        auto h = makeService();
        Setting s;
        s.id = "monitor.wallpaper.ABC";
        s.type = SettingType::String;
        s.defaultValue = std::string("");
        ASSERT_TRUE(h->service->registerSetting(s));
        ASSERT_TRUE(h->service->setValue("monitor.wallpaper.ABC",
                                         std::string("D:\\w1\\a\"b\\c.png")));
    }

    {
        auto h = makeService();
        Setting s;
        s.id = "monitor.wallpaper.ABC";
        s.type = SettingType::String;
        s.defaultValue = std::string("");
        h->service->registerSetting(s);
        auto val = h->service->getValue("monitor.wallpaper.ABC");
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(std::get<std::string>(*val), "D:\\w1\\a\"b\\c.png");
    }

    std::remove(path.c_str());
}
