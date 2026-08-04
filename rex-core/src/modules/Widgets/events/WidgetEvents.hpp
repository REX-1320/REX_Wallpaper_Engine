#pragma once

#include <string>

namespace rex::widgets {

struct WidgetCreatedEvent {
    std::string instanceId;
};

struct WidgetRemovedEvent {
    std::string instanceId;
};

struct WidgetMovedEvent {
    std::string instanceId;
    int x;
    int y;
};

struct WidgetVisibilityEvent {
    std::string instanceId;
    bool visible;
};

} // namespace rex::widgets
