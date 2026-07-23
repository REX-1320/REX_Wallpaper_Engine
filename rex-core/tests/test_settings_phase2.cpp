#include "test_settings_fixtures.hpp"

using namespace rex::settings;

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
