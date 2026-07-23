#pragma once
#include "SchemaVersion.hpp"
#include <vector>
#include <tuple>
#include <functional>

namespace rex::settings {

class SchemaManager {
public:
    using MigrationFn = std::function<bool()>;

    void addMigration(const SchemaVersion& from, const SchemaVersion& to, MigrationFn migration);
    bool migrate(const SchemaVersion& current, const SchemaVersion& target);

private:
    std::vector<std::tuple<SchemaVersion, SchemaVersion, MigrationFn>> migrations_;
};

}
