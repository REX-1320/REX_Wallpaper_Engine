#pragma once
#include <memory>
#include <string>
#include <typeindex>
#include "interfaces/IModuleContext.hpp"

namespace rex::modules {

class ServiceContainer;
class EventBus;
class ModuleRegistry;

class ModuleContext : public IModuleContext {
public:
    ModuleContext(const std::string& moduleId,
                  ServiceContainer& services,
                  ModuleRegistry& registry,
                  EventBus& bus,
                  const std::string& configPath);

    std::string getConfigPath() const override;
    void subscribeToEvents(EventCallback callback) override;
    void postEvent(const ModuleEvent& event) override;
    ServiceContainer& getServiceContainer() override { return services_; }

protected:
    std::shared_ptr<void> getServiceByType(const std::type_index& type) const override;

private:
    std::string moduleId_;
    ServiceContainer& services_;
    ModuleRegistry& registry_;
    EventBus& bus_;
    std::string configPath_;
};

} // namespace rex::modules
