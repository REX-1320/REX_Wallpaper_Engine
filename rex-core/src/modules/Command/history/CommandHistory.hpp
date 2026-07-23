#pragma once
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <optional>
#include "../models/CommandContext.hpp"
#include "../models/CommandDefinition.hpp"

namespace rex::command {

class CommandHistory {
public:
    struct Entry {
        std::string commandId;
        CommandContext contextSnapshot;
        std::function<void()> undoAction;
    };

    void push(const std::string& id, const CommandDefinition& def, const CommandContext& ctx);
    void undo();
    void redo();
    std::optional<Entry> popRedoEntry();
    bool canUndo() const;
    bool canRedo() const;
    void clear();

private:
    std::vector<Entry> undoStack_;
    std::vector<Entry> redoStack_;
    mutable std::mutex mutex_;
};

}
