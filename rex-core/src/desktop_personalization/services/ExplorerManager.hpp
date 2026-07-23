#pragma once
#include "../platform/IPlatformAdapter.hpp"
#include "modules/EventBus.hpp"
#include <memory>

namespace rex::desktop_personalization {

class ExplorerManager {
public:
    ExplorerManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                    std::shared_ptr<rex::modules::EventBus> bus);

    bool restartExplorer();
    bool refreshShell();

private:
    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
