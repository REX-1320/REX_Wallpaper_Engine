#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include "modules/ModuleState.hpp"
#include "services/SettingsService.hpp"
#include "search/SettingsSearch.hpp"
#include "history/SettingsHistory.hpp"
#include "backup/BackupManager.hpp"
#include "schema/SchemaManager.hpp"
#include "io/ImportExportManager.hpp"
#include <memory>

namespace rex::modules { class IModuleContext; }

namespace rex::settings {

class SettingsModule : public rex::modules::IModule {
public:
    SettingsModule();

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    rex::modules::IModuleContext* context_ = nullptr;

    std::unique_ptr<SettingsRegistry> registry_;
    std::unique_ptr<ValidationEngine> validator_;
    std::unique_ptr<SettingsSearch> search_;
    std::unique_ptr<SettingsHistory> history_;
    std::unique_ptr<BackupManager> backupMgr_;
    std::unique_ptr<SchemaManager> schemaMgr_;
    std::unique_ptr<ImportExportManager> ioMgr_;
    std::unique_ptr<SettingsService> service_;

    rex::modules::EventBus* bus_ = nullptr;
    std::shared_ptr<rex::modules::ConfigurationStore> configStoreShared_;
    rex::modules::ConfigurationStore* configStore_ = nullptr;
};

}
