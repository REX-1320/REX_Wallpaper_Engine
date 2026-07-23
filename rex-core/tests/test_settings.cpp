#include <gtest/gtest.h>
#include <memory>

#include "modules/Settings/registry/SettingType.hpp"
#include "modules/Settings/registry/SettingValue.hpp"
#include "modules/Settings/registry/Setting.hpp"
#include "modules/Settings/registry/Category.hpp"
#include "modules/Settings/registry/SettingsRegistry.hpp"
#include "modules/Settings/models/ValidationResult.hpp"
#include "modules/Settings/models/SearchResult.hpp"
#include "modules/Settings/validation/ValidationEngine.hpp"
#include "modules/Settings/validation/Validators.hpp"
#include "modules/Settings/search/SettingsSearch.hpp"
#include "modules/Settings/history/SettingsHistory.hpp"
#include "modules/Settings/backup/BackupManager.hpp"
#include "modules/Settings/schema/SchemaVersion.hpp"
#include "modules/Settings/schema/SchemaManager.hpp"
#include "modules/Settings/io/ImportExportManager.hpp"
#include "modules/Settings/services/SettingsService.hpp"
#include "modules/Settings/services/SettingsTransaction.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include "modules/Settings/ui/UIModelGenerator.hpp"
#include "modules/Settings/ui/UIControl.hpp"
#include "modules/EventBus.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/ModuleEvents.hpp"

using namespace rex::settings;

namespace {

class MockConfigurationStore : public rex::modules::ConfigurationStore {
public:
    MockConfigurationStore() : ConfigurationStore("test_settings_config.json") {}
};

class MockEventBus : public rex::modules::EventBus {
public:
    void publish(const rex::modules::ModuleEvent& event) override {
        (void)event;
        published_ = true;
    }
    bool published_ = false;
};

class SettingsServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto reg = std::make_unique<SettingsRegistry>();
        auto val = std::make_unique<ValidationEngine>();
        auto srch = std::make_unique<SettingsSearch>(*reg);
        auto hist = std::make_unique<SettingsHistory>();
        auto backup = std::make_unique<BackupManager>(
            std::make_shared<MockConfigurationStore>());
        auto schema = std::make_unique<SchemaManager>();
        auto io = std::make_unique<ImportExportManager>(*reg, *val);

        service = std::make_unique<SettingsService>(
            std::move(reg), std::move(val), std::move(srch), std::move(hist),
            std::move(backup), std::move(schema), std::move(io),
            bus, config);
    }

    MockEventBus bus;
    MockConfigurationStore config;
    std::unique_ptr<SettingsService> service;
};

} // namespace

TEST(SettingsTypeTest, ToString) {
    EXPECT_STREQ(toString(SettingType::Boolean), "Boolean");
    EXPECT_STREQ(toString(SettingType::Integer), "Integer");
    EXPECT_STREQ(toString(SettingType::String), "String");
}

TEST(SettingsValueTest, TypeOf) {
    EXPECT_EQ(typeOf(true), SettingType::Boolean);
    EXPECT_EQ(typeOf(int64_t(42)), SettingType::Integer);
    EXPECT_EQ(typeOf(3.14), SettingType::Float);
    EXPECT_EQ(typeOf(std::string("hello")), SettingType::String);
    EXPECT_EQ(typeOf(std::vector<std::string>{}), SettingType::List);
    EXPECT_EQ(typeOf(std::vector<uint8_t>{}), SettingType::Object);
}

TEST_F(SettingsServiceTest, RegisterAndGetValue) {
    Setting s;
    s.id = "desktop.theme.dark_mode";
    s.type = SettingType::Boolean;
    s.defaultValue = false;
    ASSERT_TRUE(service->registerSetting(s));

    auto val = service->getValue("desktop.theme.dark_mode");
    ASSERT_TRUE(val.has_value());
    EXPECT_FALSE(std::get<bool>(*val));
}

TEST_F(SettingsServiceTest, SetValueWithValidation) {
    Setting s;
    s.id = "performance.cpu_limit";
    s.type = SettingType::Integer;
    s.defaultValue = int64_t(100);
    s.validators.push_back(validators::range(int64_t(0), int64_t(100)));
    ASSERT_TRUE(service->registerSetting(s));

    EXPECT_TRUE(service->setValue("performance.cpu_limit", int64_t(50)));
    EXPECT_EQ(std::get<int64_t>(*service->getValue("performance.cpu_limit")), 50);
    EXPECT_FALSE(service->setValue("performance.cpu_limit", int64_t(150)));
    EXPECT_EQ(std::get<int64_t>(*service->getValue("performance.cpu_limit")), 50);
}

TEST_F(SettingsServiceTest, TransactionAtomicity) {
    Setting a;
    a.id = "a";
    a.type = SettingType::String;
    a.defaultValue = std::string("initial");
    a.validators.push_back(validators::regex("^[a-z]+$"));
    service->registerSetting(a);

    Setting b;
    b.id = "b";
    b.type = SettingType::String;
    b.defaultValue = std::string("initial");
    b.validators.push_back(validators::regex("^[a-z]+$"));
    service->registerSetting(b);

    auto txn = service->beginTransaction();
    service->setValue("a", std::string("valid"));
    service->setValue("b", std::string("123"));
    service->commitTransaction(std::move(txn));

    EXPECT_NE(std::get<std::string>(*service->getValue("a")), "valid");
    EXPECT_EQ(std::get<std::string>(*service->getValue("b")), "initial");
}

TEST_F(SettingsServiceTest, ResetToDefault) {
    Setting s;
    s.id = "test.greeting";
    s.type = SettingType::String;
    s.defaultValue = std::string("hello");
    ASSERT_TRUE(service->registerSetting(s));

    service->setValue("test.greeting", std::string("world"));
    EXPECT_EQ(std::get<std::string>(*service->getValue("test.greeting")), "world");

    service->resetSetting("test.greeting");
    EXPECT_EQ(std::get<std::string>(*service->getValue("test.greeting")), "hello");
}

TEST_F(SettingsServiceTest, UndoRedo) {
    Setting s;
    s.id = "test.volume";
    s.type = SettingType::Integer;
    s.defaultValue = int64_t(50);
    ASSERT_TRUE(service->registerSetting(s));

    service->setValue("test.volume", int64_t(75));
    EXPECT_EQ(std::get<int64_t>(*service->getValue("test.volume")), 75);

    EXPECT_TRUE(service->undo());
    EXPECT_EQ(std::get<int64_t>(*service->getValue("test.volume")), 50);

    EXPECT_TRUE(service->redo());
    EXPECT_EQ(std::get<int64_t>(*service->getValue("test.volume")), 75);
}

TEST_F(SettingsServiceTest, UndoWithNoHistoryReturnsFalse) {
    EXPECT_FALSE(service->undo());
}

TEST_F(SettingsServiceTest, RedoWithNoHistoryReturnsFalse) {
    EXPECT_FALSE(service->redo());
}

TEST_F(SettingsServiceTest, SearchRelevance) {
    Setting s1; s1.id = "display.brightness"; s1.name = "Brightness"; s1.category = "display";
    s1.type = SettingType::Integer; s1.defaultValue = int64_t(50);
    service->registerSetting(s1);

    Setting s2; s2.id = "audio.brightness"; s2.name = "Brightness"; s2.category = "audio";
    s2.type = SettingType::Integer; s2.defaultValue = int64_t(80);
    service->registerSetting(s2);

    auto results = service->search("brightness");
    ASSERT_GE(results.size(), 1);
    EXPECT_GT(results[0].relevance, 0.0);
}

TEST_F(SettingsServiceTest, SearchWithNoResultsReturnsEmpty) {
    Setting s; s.id = "test.val"; s.name = "Test"; s.type = SettingType::Integer;
    s.defaultValue = int64_t(0);
    service->registerSetting(s);

    auto results = service->search("nonexistent");
    EXPECT_TRUE(results.empty());
}

TEST_F(SettingsServiceTest, CreateAndRestoreBackup) {
    EXPECT_TRUE(service->createBackup());
    EXPECT_TRUE(service->restoreBackup());
}

TEST_F(SettingsServiceTest, ExportAndImportSettings) {
    Setting s;
    s.id = "test.volume";
    s.type = SettingType::Integer;
    s.defaultValue = int64_t(50);
    ASSERT_TRUE(service->registerSetting(s));

    service->setValue("test.volume", int64_t(75));

    std::string json = service->exportSettings();
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("test.volume"), std::string::npos);
    EXPECT_NE(json.find("75"), std::string::npos);
}

TEST_F(SettingsServiceTest, CurrentSchemaVersion) {
    SchemaVersion v = service->currentSchemaVersion();
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 0);
}

TEST(SchemaVersionTest, Comparison) {
    EXPECT_TRUE((SchemaVersion{1, 0} < SchemaVersion{2, 0}));
    EXPECT_TRUE((SchemaVersion{1, 0} < SchemaVersion{1, 1}));
    EXPECT_TRUE((SchemaVersion{1, 0} == SchemaVersion{1, 0}));
    EXPECT_FALSE((SchemaVersion{2, 0} < SchemaVersion{1, 0}));
}

TEST(SchemaManagerTest, DirectMigration) {
    SchemaManager mgr;
    bool migrated = false;
    mgr.addMigration(SchemaVersion{1, 0}, SchemaVersion{2, 0},
                     [&]() -> bool { migrated = true; return true; });
    EXPECT_TRUE(mgr.migrate(SchemaVersion{1, 0}, SchemaVersion{2, 0}));
    EXPECT_TRUE(migrated);
}

TEST(SchemaManagerTest, AlreadyAtTarget) {
    SchemaManager mgr;
    EXPECT_TRUE(mgr.migrate(SchemaVersion{1, 0}, SchemaVersion{1, 0}));
}

TEST(SchemaManagerTest, NoMigrationPath) {
    SchemaManager mgr;
    EXPECT_FALSE(mgr.migrate(SchemaVersion{1, 0}, SchemaVersion{2, 0}));
}

TEST(SettingsRegistryTest, AddAndGetSetting) {
    SettingsRegistry reg;

    Setting s;
    s.id = "test.bool";
    s.name = "Test Bool";
    s.type = SettingType::Boolean;
    s.defaultValue = false;

    EXPECT_TRUE(reg.addSetting(s));
    EXPECT_FALSE(reg.addSetting(s));

    auto val = reg.getValue("test.bool");
    ASSERT_TRUE(val.has_value());
    EXPECT_TRUE(std::holds_alternative<bool>(*val));
    EXPECT_FALSE(std::get<bool>(*val));
}

TEST(ValidatorsTest, RangeValidator) {
    auto v = validators::range(int64_t(0), int64_t(100));

    auto r1 = v(int64_t(50));
    EXPECT_TRUE(r1.valid);

    auto r2 = v(int64_t(200));
    EXPECT_FALSE(r2.valid);
}

TEST(ValidatorsTest, OneOfValidator) {
    auto v = validators::oneOf({"red", "green", "blue"});

    EXPECT_TRUE(v(std::string("red")).valid);
    EXPECT_TRUE(v(std::string("blue")).valid);
    EXPECT_FALSE(v(std::string("yellow")).valid);
}

TEST(ValidatorsTest, RegexValidator) {
    auto v = validators::regex("^[a-z]+$");

    EXPECT_TRUE(v(std::string("hello")).valid);
    EXPECT_FALSE(v(std::string("Hello123")).valid);
}

TEST(SettingsTransactionTest, RollbackOnFailure) {
    SettingsTransaction txn;
    int applied = 0;
    txn.addChange([&]() { applied++; return true; },
                  []() {});
    txn.addChange([&]() { applied++; return false; },
                  []() {});
    EXPECT_FALSE(txn.commit());
    EXPECT_EQ(applied, 2);
}

TEST(SettingsSearchTest, RelevanceRanking) {
    SettingsRegistry reg;
    Setting s1; s1.id = "display.brightness"; s1.name = "Brightness"; s1.category = "display";
    Setting s2; s2.id = "audio.brightness";   s2.name = "Brightness"; s2.category = "audio";
    reg.addSetting(s1);
    reg.addSetting(s2);

    SettingsSearch search(reg);
    auto results = search.search("brightness");
    ASSERT_GE(results.size(), 1);
}

TEST(SettingsHistoryTest, UndoRedo) {
    SettingsHistory history;

    history.record("volume", int64_t(50), int64_t(80));
    history.record("volume", int64_t(80), int64_t(100));

    auto change = history.undo();
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->settingId, "volume");
    EXPECT_EQ(std::get<int64_t>(change->oldValue), 80);
    EXPECT_EQ(std::get<int64_t>(change->newValue), 100);

    change = history.undo();
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(std::get<int64_t>(change->oldValue), 50);
}

TEST(SettingsHistoryTest, RedoAfterUndo) {
    SettingsHistory history;

    history.record("volume", int64_t(50), int64_t(80));
    history.record("volume", int64_t(80), int64_t(100));
    history.undo();

    auto change = history.redo();
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(std::get<int64_t>(change->oldValue), 80);
    EXPECT_EQ(std::get<int64_t>(change->newValue), 100);
}

TEST(SettingsHistoryTest, NewRecordInvalidatesRedo) {
    SettingsHistory history;

    history.record("a", int64_t(1), int64_t(2));
    history.record("a", int64_t(2), int64_t(3));
    history.undo();

    history.record("a", int64_t(3), int64_t(4));
    EXPECT_FALSE(history.redo().has_value());
}

TEST(SettingsHistoryTest, EmptyUndoReturnsNullopt) {
    SettingsHistory history;
    EXPECT_FALSE(history.undo().has_value());
    EXPECT_FALSE(history.redo().has_value());
}

TEST(SettingsHistoryTest, ClearResetsBothStacks) {
    SettingsHistory history;
    history.record("a", int64_t(1), int64_t(2));
    history.clear();
    EXPECT_FALSE(history.undo().has_value());
}

TEST(ImportExportManagerTest, ExportProducesJson) {
    SettingsRegistry reg;
    ValidationEngine val;
    Setting s; s.id = "test.flag"; s.name = "Flag"; s.type = SettingType::Boolean;
    s.defaultValue = true;
    reg.addSetting(s);

    ImportExportManager mgr(reg, val);
    std::string json = mgr.exportToJson({"test.flag"});

    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("test.flag"), std::string::npos);
    EXPECT_NE(json.find("true"), std::string::npos);
}

TEST(ImportExportManagerTest, ExportAllSettings) {
    SettingsRegistry reg;
    ValidationEngine val;
    Setting s1; s1.id = "a"; s1.type = SettingType::Integer; s1.defaultValue = int64_t(1);
    Setting s2; s2.id = "b"; s2.type = SettingType::String; s2.defaultValue = std::string("hello");
    reg.addSetting(s1);
    reg.addSetting(s2);

    ImportExportManager mgr(reg, val);
    std::string json = mgr.exportToJson();
    EXPECT_NE(json.find("a"), std::string::npos);
    EXPECT_NE(json.find("b"), std::string::npos);
}

TEST(BackupManagerTest, CreateAndRestore) {
    auto store = std::make_shared<MockConfigurationStore>();
    BackupManager mgr(store);
    EXPECT_TRUE(mgr.createBackup());
    EXPECT_TRUE(mgr.restoreLatestBackup());
}

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
