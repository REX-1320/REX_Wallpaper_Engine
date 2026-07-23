#pragma once
#include <string>

namespace rex::ui {

enum class TokenCategory {
    Color,
    Typography,
    Spacing,
    Radius,
    Shadow,
    Blur,
    Motion,
    Opacity,
    Elevation,
    Border
};

using TokenValue = std::string;

}
