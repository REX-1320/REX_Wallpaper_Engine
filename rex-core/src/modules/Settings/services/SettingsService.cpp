#include "SettingsService.hpp"
#include "../interfaces/ISettingsProvider.hpp"
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <utility>

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

void SettingsService::persistSetting(const std::string& /*id*/) {
    // Persist the whole registry (few settings; avoids per-key writes).
    // The payload is a JSON object of id -> value, stored on the
    // "rex.settings" module config through the ConfigurationStore.
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& s : registry_->getAllSettings()) {
        std::string val = valueToJson(s.currentValue);
        if (val.empty()) continue;
        if (!first) out << ",";
        first = false;
        out << "\"" << jsonEscape(s.id) << "\":" << val;
    }
    out << "}";

    try {
        rex::modules::ModuleConfig cfg = configStore_.loadModule("rex.settings");
        cfg.settingsJson = out.str();
        configStore_.saveModule(cfg);
    } catch (const std::exception& e) {
        // Persistence is best-effort; a failing write must not break the
        // caller (settings are already applied in-memory).
        (void)e;
    }
}

void SettingsService::hydrateSettings() {
    rex::modules::ModuleConfig cfg;
    try {
        cfg = configStore_.loadModule("rex.settings");
    } catch (const std::exception&) {
        return;
    }
    if (cfg.settingsJson.empty()) return;

    for (auto& [id, value] : parseSettingsObject(cfg.settingsJson)) {
        Setting s;
        s.id = id;
        s.name = id;
        s.type = typeOf(value);
        s.defaultValue = value;
        registry_->addSetting(s); // ignored when already registered
    }
}

std::string SettingsService::valueToJson(const SettingValue& value) {
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    }
    if (std::holds_alternative<int64_t>(value)) {
        return std::to_string(std::get<int64_t>(value));
    }
    if (std::holds_alternative<double>(value)) {
        std::ostringstream os;
        os << std::get<double>(value);
        return os.str();
    }
    if (std::holds_alternative<std::string>(value)) {
        return "\"" + jsonEscape(std::get<std::string>(value)) + "\"";
    }
    if (std::holds_alternative<std::vector<std::string>>(value)) {
        std::ostringstream os;
        os << "[";
        bool first = true;
        for (const auto& item : std::get<std::vector<std::string>>(value)) {
            if (!first) os << ",";
            first = false;
            os << "\"" << jsonEscape(item) << "\"";
        }
        os << "]";
        return os.str();
    }
    return ""; // vector<uint8_t> blobs are not JSON-serializable here
}

namespace {

// Index of the first unescaped '"' at/after `from` in a JSON string scan.
size_t findUnescapedQuote(const std::string& s, size_t from) {
    for (size_t i = from; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == '"') return i;
    }
    return std::string::npos;
}

} // namespace

std::vector<std::pair<std::string, SettingValue>> SettingsService::parseSettingsObject(
    const std::string& json) {
    std::vector<std::pair<std::string, SettingValue>> out;
    size_t i = 0;
    while (i < json.size()) {
        // Skip to the next quoted key.
        auto keyStart = json.find('"', i);
        if (keyStart == std::string::npos) break;
        auto keyEnd = findUnescapedQuote(json, keyStart + 1);
        if (keyEnd == std::string::npos) break;
        std::string key = jsonUnescape(json.substr(keyStart + 1,
                                                   keyEnd - keyStart - 1));
        i = keyEnd + 1;
        auto colon = json.find(':', i);
        if (colon == std::string::npos) break;
        i = colon + 1;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size()) break;

        if (json[i] == '"') {
            auto vEnd = findUnescapedQuote(json, i + 1);
            if (vEnd == std::string::npos) break;
            out.emplace_back(key, SettingValue(
                jsonUnescape(json.substr(i + 1, vEnd - i - 1))));
            i = vEnd + 1;
        } else if (json.compare(i, 4, "true") == 0) {
            out.emplace_back(key, SettingValue(true));
            i += 4;
        } else if (json.compare(i, 5, "false") == 0) {
            out.emplace_back(key, SettingValue(false));
            i += 5;
        } else {
            // Number: consume until , or }.
            auto vEnd = json.find_first_of(",}", i);
            if (vEnd == std::string::npos) vEnd = json.size();
            std::string num = json.substr(i, vEnd - i);
            try {
                size_t pos = 0;
                long long ll = std::stoll(num, &pos);
                if (pos == num.size()) {
                    out.emplace_back(key, SettingValue(static_cast<int64_t>(ll)));
                } else {
                    out.emplace_back(key, SettingValue(std::stod(num)));
                }
            } catch (const std::exception&) {
                // Unparsable token: skip.
            }
            i = vEnd;
        }
    }
    return out;
}

std::string SettingsService::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string SettingsService::jsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            default:   out += s[i];
        }
    }
    return out;
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
