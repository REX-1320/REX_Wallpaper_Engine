#pragma once

#include <string>
#include <functional>
#include "../models/WidgetDefinition.hpp"

namespace rex::widgets {

class IWidgetProvider {
public:
    virtual ~IWidgetProvider() = default;

    virtual WidgetDefinition getDefinition() const = 0;
    virtual std::string render() const = 0;
    virtual void onMessage(const std::string& message) = 0;
    virtual void onTick(double deltaTime) = 0;
};

} // namespace rex::widgets
