#pragma once
#include "../interfaces/ISettingsService.hpp"
#include "../registry/SettingsRegistry.hpp"
#include "../validation/ValidationEngine.hpp"
#include "../services/SettingsTransaction.hpp"
#include "../search/SettingsSearch.hpp"
#include "../history/SettingsHistory.hpp"
#include "../models/SearchResult.hpp"
#include "../backup/BackupManager.hpp"
#include "../schema/SchemaManager.hpp"
#include "../io/ImportExportManager.hpp"
#include "../ui/UIModelGenerator.hpp"
#include "../ui/UIControl.hpp"
#include "modules/EventBus.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace rex::settings {

class SettingsService : public ISettingsService {
public:
    SettingsService(std::unique_ptr<SettingsRegistry> registry,
                    std::unique_ptr<ValidationEngine> validator,
                    std::unique_ptr<SettingsSearch> search,
                    std::unique_ptr<SettingsHistory> history,
                    std::unique_ptr<BackupManager> backupMgr,
                    std::unique_ptr<SchemaManager> schemaMgr,
                    std::unique_ptr<ImportExportManager> ioMgr,
                    rex::modules::EventBus& bus,
                    rex::modules::ConfigurationStore& configStore);

    bool registerSetting(const Setting& setting) override;
    bool unregisterSetting(const std::string& id) override;
    bool registerCategory(const Category& category) override;

    std::optional<SettingValue> getValue(const std::string& id) override;
    bool setValue(const std::string& id, const SettingValue& value) override;
    bool resetSetting(const std::string& id) override;
    bool resetCategory(const std::string& categoryId) override;

    ValidationResult validate(const std::string& id, const SettingValue& value) override;

    std::unique_ptr<SettingsTransaction> beginTransaction() override;
    void commitTransaction(std::unique_ptr<SettingsTransaction> txn) override;
    void rollbackTransaction(std::unique_ptr<SettingsTransaction> txn) override;

    std::vector<Setting> getAllSettings() override;
    std::vector<Category> getAllCategories() override;

    std::vector<SearchResult> search(const std::string& query,
                                     const std::string& category = "",
                                     int maxResults = 20);
    bool undo();
    bool redo();
    void discoverProviders(rex::modules::IModuleContext* context);

    /// Load persisted settings (the "rex.settings" config blob) into the
    /// registry. Idempotent: already-registered ids are left untouched.
    void hydrateSettings();

    bool createBackup() override;
    bool restoreBackup(const std::string& timestamp = "") override;
    std::string exportSettings(const std::vector<std::string>& ids = {}) override;
    bool importSettings(const std::string& json, bool overwrite = true) override;
    SchemaVersion currentSchemaVersion() override;
    bool migrateTo(const SchemaVersion& target) override;

    std::vector<ui::UIControl> getUIControls(const std::string& category = "") const;

private:
    bool internalSetValue(const std::string& id, const SettingValue& value, bool recordHistory = true);
    void rollbackPending();
    void persistSetting(const std::string& id);
    void publishEvent(const std::string& id);

    // ─── Persistence helpers ────────────────────────────────────────────
    static std::string valueToJson(const SettingValue& value);
    static std::vector<std::pair<std::string, SettingValue>>
        parseSettingsObject(const std::string& json);
    static std::string jsonEscape(const std::string& s);
    static std::string jsonUnescape(const std::string& s);

    std::unique_ptr<SettingsRegistry> registry_;
    std::unique_ptr<ValidationEngine> validator_;
    std::unique_ptr<SettingsSearch> search_;
    std::unique_ptr<SettingsHistory> history_;
    std::unique_ptr<BackupManager> backupManager_;
    std::unique_ptr<SchemaManager> schemaManager_;
    std::unique_ptr<ImportExportManager> ioManager_;
    rex::modules::EventBus& bus_;
    rex::modules::ConfigurationStore& configStore_;
    SettingsTransaction* activeTransaction_ = nullptr;
    std::vector<std::string> pendingEventIds_;
    std::unordered_map<std::string, SettingValue> pendingUndos_;
};

}
