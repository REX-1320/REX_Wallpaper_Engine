#include "BackupManager.hpp"
#include <ctime>

namespace rex::settings {

BackupManager::BackupManager(std::shared_ptr<rex::modules::ConfigurationStore> store)
    : store_(std::move(store)) {}

bool BackupManager::createBackup() {
    auto timestamp = std::to_string(std::time(nullptr));
    (void)timestamp;
    return true;
}

bool BackupManager::restoreLatestBackup() {
    return true;
}

}
