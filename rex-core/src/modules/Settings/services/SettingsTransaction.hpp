#pragma once
#include <vector>
#include <functional>
#include <stdexcept>

namespace rex::settings {

class SettingsTransaction {
public:
    using ApplyFunc = std::function<bool()>;
    using UndoFunc = std::function<void()>;

    void addChange(ApplyFunc apply, UndoFunc undo);
    bool commit();
    void rollback();

private:
    struct Change {
        ApplyFunc apply;
        UndoFunc undo;
    };
    std::vector<Change> changes_;
    std::vector<UndoFunc> appliedUndos_;
    bool committed_ = false;
};

} // namespace rex::settings
