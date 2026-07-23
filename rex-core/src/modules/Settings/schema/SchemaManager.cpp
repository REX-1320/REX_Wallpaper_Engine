#include "SchemaManager.hpp"

namespace rex::settings {

void SchemaManager::addMigration(const SchemaVersion& from, const SchemaVersion& to, MigrationFn fn) {
    migrations_.emplace_back(from, to, std::move(fn));
}

bool SchemaManager::migrate(const SchemaVersion& current, const SchemaVersion& target) {
    if (current == target) return true;
    for (auto& [f, t, fn] : migrations_) {
        if (f == current && t == target) return fn();
    }
    return false;
}

}
