#include "SettingsService.hpp"
#include "../interfaces/ISettingsProvider.hpp"
#include <stdexcept>

namespace rex::settings {

SettingsService::SettingsService(std::unique_ptr<SettingsRegistry> registry,
                                 std::unique_ptr<ValidationEngine> validator,
                                 std::unique_ptr<SettingsSearch> search,
                                 std::unique_ptr<SettingsHistory> history,
                                 std::unique_ptr<BackupManager> backupMgr,
                                 std::unique_ptr<SchemaManager> schemaMgr,
                                 std::unique_ptr<ImportExportManager> ioMgr,
                                 rex::modules::EventBus& bus,
                                 rex::modules::ConfigurationStore& configStore)
    : registry_(std::move(registry)), validator_(std::move(validator)),
      search_(std::move(search)), history_(std::move(history)),
      backupManager_(std::move(backupMgr)), schemaManager_(std::move(schemaMgr)),
      ioManager_(std::move(ioMgr)),
      bus_(bus), configStore_(configStore) {}

bool SettingsService::registerSetting(const Setting& setting) {
    return registry_->addSetting(setting);
}

bool SettingsService::unregisterSetting(const std::string& id) {
    return registry_->removeSetting(id);
}

bool SettingsService::registerCategory(const Category& category) {
    return registry_->addCategory(category);
}

std::optional<SettingValue> SettingsService::getValue(const std::string& id) {
    return registry_->getValue(id);
}

bool SettingsService::setValue(const std::string& id, const SettingValue& value) {
    if (activeTransaction_) {
        if (!internalSetValue(id, value)) {
            rollbackPending();
            return false;
        }
        return true;
    }

    auto txn = std::make_unique<SettingsTransaction>();
    activeTransaction_ = txn.get();
    bool ok = internalSetValue(id, value);
    if (ok) {
        txn->addChange([this]() {
            for (const auto& eid : pendingEventIds_) persistSetting(eid);
            return true;
        }, [](){});
        txn->commit();
        for (const auto& eid : pendingEventIds_) publishEvent(eid);
    } else {
        rollbackPending();
        txn->rollback();
    }
    activeTransaction_ = nullptr;
    pendingEventIds_.clear();
    pendingUndos_.clear();
    return ok;
}

bool SettingsService::internalSetValue(const std::string& id, const SettingValue& value, bool recordHistory) {
    const Setting* setting = registry_->getSetting(id);
    if (!setting) return false;

    ValidationResult vr = validator_->validate(*setting, value);
    if (!vr.valid) return false;

    if (recordHistory) {
        auto oldVal = registry_->getValue(id);
        if (oldVal.has_value()) {
            history_->record(id, *oldVal, value);
        }
    }

    pendingUndos_.try_emplace(id, setting->currentValue);
    if (!registry_->setValue(id, value)) return false;

    pendingEventIds_.push_back(id);
    return true;
}

void SettingsService::rollbackPending() {
    for (auto it = pendingUndos_.begin(); it != pendingUndos_.end(); ++it) {
        registry_->setValue(it->first, it->second);
    }
}

bool SettingsService::resetSetting(const std::string& id) {
    const Setting* setting = registry_->getSetting(id);
    if (!setting) return false;
    return setValue(id, setting->defaultValue);
}

bool SettingsService::resetCategory(const std::string& categoryId) {
    auto settings = registry_->getSettingsByCategory(categoryId);
    auto txn = beginTransaction();
    for (const auto& s : settings) {
        if (!internalSetValue(s.id, s.defaultValue)) {
            rollbackTransaction(std::move(txn));
            return false;
        }
    }
    commitTransaction(std::move(txn));
    return true;
}

ValidationResult SettingsService::validate(const std::string& id, const SettingValue& value) {
    const Setting* setting = registry_->getSetting(id);
    if (!setting) return {false, "Setting not found"};
    return validator_->validate(*setting, value);
}

std::unique_ptr<SettingsTransaction> SettingsService::beginTransaction() {
    if (activeTransaction_) return nullptr;
    auto txn = std::make_unique<SettingsTransaction>();
    activeTransaction_ = txn.get();
    return txn;
}

void SettingsService::commitTransaction(std::unique_ptr<SettingsTransaction> txn) {
    if (txn.get() != activeTransaction_) return;
    txn->addChange([this]() {
        for (const auto& id : pendingEventIds_) persistSetting(id);
        return true;
    }, [](){});
    txn->commit();
    for (const auto& id : pendingEventIds_) publishEvent(id);
    activeTransaction_ = nullptr;
    pendingEventIds_.clear();
    pendingUndos_.clear();
}

void SettingsService::rollbackTransaction(std::unique_ptr<SettingsTransaction> txn) {
    if (txn.get() != activeTransaction_) return;
    rollbackPending();
    txn->rollback();
    activeTransaction_ = nullptr;
    pendingEventIds_.clear();
    pendingUndos_.clear();
}

void SettingsService::persistSetting(const std::string& id) {
    (void)id;
}

void SettingsService::publishEvent(const std::string& id) {
    (void)id;
    bus_.publish(rex::modules::ModuleEvent{
        rex::modules::ModuleEventType::SettingChanged,
        "rex.settings",
        rex::modules::ModuleState::Running,
        rex::modules::ModuleState::Running
    });
}

std::vector<Setting> SettingsService::getAllSettings() { return registry_->getAllSettings(); }
std::vector<Category> SettingsService::getAllCategories() { return registry_->getAllCategories(); }

std::vector<SearchResult> SettingsService::search(const std::string& query,
                                                  const std::string& category,
                                                  int maxResults) {
    return search_->search(query, category, maxResults);
}

bool SettingsService::undo() {
    auto change = history_->undo();
    if (!change) return false;
    return internalSetValue(change->settingId, change->oldValue, false);
}

bool SettingsService::redo() {
    auto change = history_->redo();
    if (!change) return false;
    return internalSetValue(change->settingId, change->newValue, false);
}

void SettingsService::discoverProviders(rex::modules::IModuleContext* context) {
    (void)context;
}

bool SettingsService::createBackup() {
    return backupManager_->createBackup();
}

bool SettingsService::restoreBackup(const std::string& timestamp) {
    (void)timestamp;
    return backupManager_->restoreLatestBackup();
}

std::string SettingsService::exportSettings(const std::vector<std::string>& ids) {
    return ioManager_->exportToJson(ids);
}

bool SettingsService::importSettings(const std::string& json, bool overwrite) {
    auto txn = beginTransaction();
    bool ok = ioManager_->importFromJson(json, overwrite, *this);
    if (ok) commitTransaction(std::move(txn));
    else    rollbackTransaction(std::move(txn));
    return ok;
}

SchemaVersion SettingsService::currentSchemaVersion() {
    return SchemaVersion{1, 0};
}

bool SettingsService::migrateTo(const SchemaVersion& target) {
    SchemaVersion current = currentSchemaVersion();
    return schemaManager_->migrate(current, target);
}

std::vector<ui::UIControl> SettingsService::getUIControls(const std::string& category) const {
    std::vector<Setting> settings;
    if (category.empty()) {
        settings = registry_->getAllSettings();
    } else {
        settings = registry_->getSettingsByCategory(category);
    }
    return UIModelGenerator::generate(settings);
}

}
