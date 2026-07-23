#pragma once
#include <string>
#include <functional>

namespace rex::notifications {
struct NotificationAction {
    std::string id;
    std::string label;
    bool enabled = true;
    std::function<void(const std::string&)> callback;
};
}
