#pragma once
#include <gtest/gtest.h>
#include <memory>

#include "modules/Settings/registry/SettingsRegistry.hpp"
#include "modules/Settings/validation/ValidationEngine.hpp"
#include "modules/Settings/search/SettingsSearch.hpp"
#include "modules/Settings/history/SettingsHistory.hpp"
#include "modules/Settings/backup/BackupManager.hpp"
#include "modules/Settings/io/ImportExportManager.hpp"
#include "modules/Settings/schema/SchemaManager.hpp"
#include "modules/Settings/services/SettingsService.hpp"
#include "modules/Settings/validation/Validators.hpp"
#include "modules/EventBus.hpp"
#include "modules/ConfigurationStore.hpp"

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

}
