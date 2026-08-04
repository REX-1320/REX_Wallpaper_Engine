#include "WidgetRuntime.hpp"
#include "WidgetHost.hpp"
#include "WidgetRegistry.hpp"

namespace rex::widgets {

WidgetRuntime::WidgetRuntime(std::shared_ptr<WidgetRegistry> registry,
                             std::shared_ptr<WidgetHost> host)
    : m_registry(registry), m_host(host) {
}

bool WidgetRuntime::spawn(const std::string& widgetDefId, int x, int y) {
    auto* def = m_registry->get(widgetDefId);
    if (!def) return false;

    WidgetInstance inst;
    inst.id = widgetDefId + "_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    inst.widgetId = widgetDefId;
    inst.x = x;
    inst.y = y;
    inst.width = def->defaultWidth;
    inst.height = def->defaultHeight;

    return m_host->createWindow(inst, *def);
}

void WidgetRuntime::kill(const std::string& instanceId) {
    m_host->destroyWindow(instanceId);
}

void WidgetRuntime::updateAll(float deltaTime) {
    (void)deltaTime;
}

void WidgetRuntime::setMessageHandler(MessageHandler handler) {
    m_messageHandler = handler;
}

} // namespace rex::widgets
