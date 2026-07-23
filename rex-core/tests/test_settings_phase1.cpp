#include "test_settings_fixtures.hpp"

using namespace rex::settings;

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
