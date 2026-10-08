#include "SettingsModule.hpp"
#include <shared/Logger.hpp>
#include "interfaces/ISettingsService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/interfaces/IModuleContext.hpp"

namespace rex::settings {

SettingsModule::SettingsModule() {
    metadata_.id = "rex.settings";
    metadata_.name = "Settings";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

bool SettingsModule::initialize(rex::modules::IModuleContext* context) {
    context_ = context;
    bus_ = context->getService<rex::modules::EventBus>().get();
    configStoreShared_ = context->getService<rex::modules::ConfigurationStore>();
    configStore_ = configStoreShared_.get();

    // Both are dereferenced below (and by every service that takes them by
    // reference). Bail out instead of dereferencing nullptr: a missing service
    // is an orchestration bug, but crashing the host with an access violation
    // hides it completely.
    if (!bus_ || !configStore_) {
        REX_LOG_ERROR("Settings",
                      "Core services missing from the container "
                      "(EventBus={}, ConfigurationStore={}) — cannot initialize",
                      bus_ ? "ok" : "null", configStore_ ? "ok" : "null");
        state_ = rex::modules::ModuleState::Failed;
        return false;
    }

    registry_ = std::make_unique<SettingsRegistry>();
    validator_ = std::make_unique<ValidationEngine>();
    search_ = std::make_unique<SettingsSearch>(*registry_);
    history_ = std::make_unique<SettingsHistory>();
    backupMgr_ = std::make_unique<BackupManager>(configStoreShared_);
    schemaMgr_ = std::make_unique<SchemaManager>();
    ioMgr_ = std::make_unique<ImportExportManager>(*registry_, *validator_);

    service_ = std::make_unique<SettingsService>(
        std::move(registry_),
        std::move(validator_),
        std::move(search_),
        std::move(history_),
        std::move(backupMgr_),
        std::move(schemaMgr_),
        std::move(ioMgr_),
        *bus_,
        *configStore_);

    // Load persisted settings (id -> value blob) into the registry so
    // getValue() returns them from the first call.
    service_->hydrateSettings();

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool SettingsModule::start() {
    service_->discoverProviders(context_);
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void SettingsModule::stop() { state_ = rex::modules::ModuleState::Stopped; }
void SettingsModule::shutdown() { state_ = rex::modules::ModuleState::Unloaded; }

}
