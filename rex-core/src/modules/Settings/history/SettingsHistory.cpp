#include "SettingsHistory.hpp"

namespace rex::settings {

void SettingsHistory::record(const std::string& id, const SettingValue& oldVal, const SettingValue& newVal) {
    past_.push_back({id, oldVal, newVal});
    future_.clear();
}

std::optional<SettingsHistory::Change> SettingsHistory::undo() {
    if (past_.empty()) return std::nullopt;
    Change c = past_.back();
    past_.pop_back();
    future_.push_back(c);
    return c;
}

std::optional<SettingsHistory::Change> SettingsHistory::redo() {
    if (future_.empty()) return std::nullopt;
    Change c = future_.back();
    future_.pop_back();
    past_.push_back(c);
    return c;
}

void SettingsHistory::clear() {
    past_.clear();
    future_.clear();
}

}
