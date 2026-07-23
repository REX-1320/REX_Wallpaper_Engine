#include "SettingsTransaction.hpp"

namespace rex::settings {

void SettingsTransaction::addChange(ApplyFunc apply, UndoFunc undo) {
    if (committed_) throw std::logic_error("Transaction already committed");
    changes_.push_back({std::move(apply), std::move(undo)});
}

bool SettingsTransaction::commit() {
    committed_ = true;
    for (auto& ch : changes_) {
        if (!ch.apply()) {
            for (auto it = appliedUndos_.rbegin(); it != appliedUndos_.rend(); ++it) {
                (*it)();
            }
            return false;
        }
        appliedUndos_.push_back(std::move(ch.undo));
    }
    return true;
}

void SettingsTransaction::rollback() {
    for (auto it = appliedUndos_.rbegin(); it != appliedUndos_.rend(); ++it) {
        (*it)();
    }
    appliedUndos_.clear();
}

} // namespace rex::settings
