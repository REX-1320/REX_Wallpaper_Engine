#pragma once

#include <string>

namespace rex::widgets {

struct WidgetDefinition {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string type;
    std::string entryPoint;
    std::string icon;
    int defaultWidth = 200;
    int defaultHeight = 200;
};

} // namespace rex::widgets
