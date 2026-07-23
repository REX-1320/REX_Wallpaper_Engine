#include "CommandHistory.hpp"

namespace rex::command {

void CommandHistory::push(const std::string& id, const CommandDefinition& def, const CommandContext& ctx) {
    if (!def.undo) return;
    std::lock_guard lock(mutex_);
    Entry entry;
    entry.commandId = id;
    entry.contextSnapshot = ctx;
    entry.undoAction = [&def, ctxCopy = ctx]() mutable {
        auto mutableCtx = ctxCopy;
        def.undo(mutableCtx);
    };
    undoStack_.push_back(std::move(entry));
    redoStack_.clear();
}

void CommandHistory::undo() {
    std::lock_guard lock(mutex_);
    if (undoStack_.empty()) return;
    Entry entry = undoStack_.back();
    undoStack_.pop_back();
    if (entry.undoAction) {
        entry.undoAction();
    }
    redoStack_.push_back(std::move(entry));
}

void CommandHistory::redo() {
    std::lock_guard lock(mutex_);
    if (redoStack_.empty()) return;
    Entry entry = redoStack_.back();
    redoStack_.pop_back();
    undoStack_.push_back(std::move(entry));
}

std::optional<CommandHistory::Entry> CommandHistory::popRedoEntry() {
    std::lock_guard lock(mutex_);
    if (redoStack_.empty()) return std::nullopt;
    Entry entry = std::move(redoStack_.back());
    redoStack_.pop_back();
    return entry;
}

bool CommandHistory::canUndo() const {
    std::lock_guard lock(mutex_);
    return !undoStack_.empty();
}

bool CommandHistory::canRedo() const {
    std::lock_guard lock(mutex_);
    return !redoStack_.empty();
}

void CommandHistory::clear() {
    std::lock_guard lock(mutex_);
    undoStack_.clear();
    redoStack_.clear();
}

}
