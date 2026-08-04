#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <memory>

#include "../models/WidgetInstance.hpp"
#include "../models/WidgetDefinition.hpp"

namespace rex::widgets {

class WidgetHost;
class WidgetRegistry;

class WidgetRuntime {
public:
    WidgetRuntime(std::shared_ptr<WidgetRegistry> registry,
                  std::shared_ptr<WidgetHost> host);

    bool spawn(const std::string& widgetDefId, int x, int y);
    void kill(const std::string& instanceId);
    void updateAll(float deltaTime);

    using MessageHandler = std::function<void(const std::string& instanceId, const std::string& message)>;
    void setMessageHandler(MessageHandler handler);

private:
    std::shared_ptr<WidgetRegistry> m_registry;
    std::shared_ptr<WidgetHost> m_host;
    MessageHandler m_messageHandler;
};

} // namespace rex::widgets
