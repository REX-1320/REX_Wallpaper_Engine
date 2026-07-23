#pragma once
#include "IThemeService.hpp"

namespace rex::ui {

class IThemeProvider {
public:
    virtual ~IThemeProvider() = default;
    virtual void registerThemes(IThemeService& service) = 0;
};

}
