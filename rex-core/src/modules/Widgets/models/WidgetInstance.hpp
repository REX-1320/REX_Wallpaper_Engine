#pragma once

#include <string>

namespace rex::widgets {

struct WidgetInstance {
    std::string id;
    std::string widgetId;
    int x = 100;
    int y = 100;
    int width = 200;
    int height = 200;
    bool visible = true;
    bool clickThrough = true;
    float opacity = 1.0f;
};

} // namespace rex::widgets
