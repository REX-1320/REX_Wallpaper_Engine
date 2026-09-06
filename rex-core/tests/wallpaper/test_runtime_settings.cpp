#include <gtest/gtest.h>
#include "wallpaper/runtime/RuntimeSettings.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace rex::wallpaper {
namespace {

class RuntimeSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_testFile = fs::temp_directory_path() / "test_settings_rex.json";
        fs::remove(m_testFile);
    }

    void TearDown() override {
        fs::remove(m_testFile);
    }

    fs::path m_testFile;
};

TEST_F(RuntimeSettingsTest, RT01_LoadFlatJson) {
    {
        std::ofstream ofs(m_testFile);
        ofs << R"({"testKey":"testValue"})";
    }

    RuntimeSettings settings(m_testFile);
    EXPECT_TRUE(settings.Load());
    EXPECT_EQ(settings.GetString("testKey"), "testValue");
}

TEST_F(RuntimeSettingsTest, RT02_SaveAndReload) {
    {
        RuntimeSettings settings(m_testFile);
        settings.SetString("savedKey", "savedValue");
        settings.Save();
    }

    RuntimeSettings reloaded(m_testFile);
    EXPECT_TRUE(reloaded.Load());
    EXPECT_EQ(reloaded.GetString("savedKey"), "savedValue");
}

TEST_F(RuntimeSettingsTest, RT03_MissingFileReturnsFalse) {
    RuntimeSettings settings(m_testFile);
    // Load on non-existent file should return false (empty state)
    EXPECT_FALSE(settings.Load());
}

TEST_F(RuntimeSettingsTest, RT04_DefaultValues) {
    RuntimeSettings settings(m_testFile);
    EXPECT_EQ(settings.GetString("nonexistent", "default"), "default");
    EXPECT_EQ(settings.GetString("nonexistent"), "");
}

TEST_F(RuntimeSettingsTest, RT05_MultipleKeys) {
    RuntimeSettings settings(m_testFile);
    settings.SetString("key1", "value1");
    settings.SetString("key2", "value2");
    settings.SetString("key3", "value3");

    EXPECT_EQ(settings.GetString("key1"), "value1");
    EXPECT_EQ(settings.GetString("key2"), "value2");
    EXPECT_EQ(settings.GetString("key3"), "value3");
}

TEST_F(RuntimeSettingsTest, RT06_EmptyFile) {
    {
        std::ofstream ofs(m_testFile);
        ofs << "{}";
    }

    RuntimeSettings settings(m_testFile);
    EXPECT_TRUE(settings.Load());
}

TEST_F(RuntimeSettingsTest, RT07_RemoveKey) {
    RuntimeSettings settings(m_testFile);
    settings.SetString("toRemove", "value");
    EXPECT_EQ(settings.GetString("toRemove"), "value");

    settings.RemoveKey("toRemove");
    EXPECT_EQ(settings.GetString("toRemove"), "");
}

TEST_F(RuntimeSettingsTest, RT08_MonitorWallpaper) {
    RuntimeSettings settings(m_testFile);
    uint64_t edidHash = 0x1234567890ABCDEF;
    settings.SetMonitorWallpaper(edidHash, "C:\\wallpapers\\monitor1.jpg");

    EXPECT_EQ(settings.GetMonitorWallpaper(edidHash), "C:\\wallpapers\\monitor1.jpg");
    EXPECT_EQ(settings.GetMonitorWallpaper(0xFFFFFFFF), "");
}

TEST_F(RuntimeSettingsTest, RT09_RemoveMonitorWallpaper) {
    RuntimeSettings settings(m_testFile);
    uint64_t edidHash = 0x1234567890ABCDEF;
    settings.SetMonitorWallpaper(edidHash, "path");
    settings.RemoveMonitorWallpaper(edidHash);
    EXPECT_EQ(settings.GetMonitorWallpaper(edidHash), "");
}

TEST_F(RuntimeSettingsTest, RT10_GetKeysByPrefix) {
    RuntimeSettings settings(m_testFile);
    settings.SetString("monitor:abc", "path1");
    settings.SetString("monitor:def", "path2");
    settings.SetString("other", "path3");

    auto keys = settings.GetKeysByPrefix("monitor:");
    EXPECT_EQ(keys.size(), 2u);
}

} // namespace
} // namespace rex::wallpaper
