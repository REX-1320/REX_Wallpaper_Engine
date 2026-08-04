#pragma once

#include <string>
#include <vector>
#include "../models/WidgetInstance.hpp"

namespace rex::widgets {

class IWidgetService {
public:
    virtual ~IWidgetService() = default;

    virtual std::string createWidget(const std::string& widgetDefId, int x, int y) = 0;
    virtual void removeWidget(const std::string& instanceId) = 0;
    virtual void moveWidget(const std::string& instanceId, int x, int y) = 0;
    virtual void setVisible(const std::string& instanceId, bool visible) = 0;
    virtual std::vector<WidgetInstance> getInstances() const = 0;
};

} // namespace rex::widgets
