#pragma once
#include <string>
#include <memory>
#include "modules/ConfigurationStore.hpp"

namespace rex::settings {

class BackupManager {
public:
    explicit BackupManager(std::shared_ptr<rex::modules::ConfigurationStore> store);

    bool createBackup();
    bool restoreLatestBackup();

private:
    std::shared_ptr<rex::modules::ConfigurationStore> store_;
};

}
