#include "ModuleContext.hpp"
#include "ServiceContainer.hpp"
#include "EventBus.hpp"
#include "ModuleEvents.hpp"

namespace rex::modules {

ModuleContext::ModuleContext(const std::string& moduleId,
                             ServiceContainer& services,
                             ModuleRegistry& registry,
                             EventBus& bus,
                             const std::string& configPath)
    : moduleId_(moduleId), services_(services), registry_(registry),
      bus_(bus), configPath_(configPath) {}

std::string ModuleContext::getConfigPath() const {
    return configPath_;
}

void ModuleContext::subscribeToEvents(EventCallback callback) {
    bus_.subscribe(std::move(callback));
}

void ModuleContext::postEvent(const ModuleEvent& event) {
    bus_.publish(event);
}

std::shared_ptr<void> ModuleContext::getServiceByType(const std::type_index& type) const {
    return services_.findService(type);
}

} // namespace rex::modules
