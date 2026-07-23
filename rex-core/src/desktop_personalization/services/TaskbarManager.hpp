#pragma once
#include "../platform/IPlatformAdapter.hpp"
#include "modules/EventBus.hpp"
#include <memory>

namespace rex::desktop_personalization {

class TaskbarManager {
public:
    TaskbarManager(std::shared_ptr<platform::IPlatformAdapter> platform,
                   std::shared_ptr<rex::modules::EventBus> bus);

    bool setAutoHide(bool autoHide);
    bool setAlignment(int alignment);
    bool refreshTaskbar();

private:
    std::shared_ptr<platform::IPlatformAdapter> platform_;
    std::shared_ptr<rex::modules::EventBus> bus_;
};

} // namespace rex::desktop_personalization
