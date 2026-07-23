#pragma once

#include <string>
#include <functional>
#include <memory>
#include <typeindex>

namespace rex::modules {

class ServiceContainer;
enum class ModuleEventType : int;
struct ModuleEvent;

class IModuleContext {
public:
    virtual ~IModuleContext() = default;

    virtual std::string getConfigPath() const = 0;

    using EventCallback = std::function<void(const ModuleEvent&)>;
    virtual void subscribeToEvents(EventCallback callback) = 0;
    virtual void postEvent(const ModuleEvent& event) = 0;

    template<typename T>
    std::shared_ptr<T> getService() const {
        auto svc = getServiceByType(std::type_index(typeid(T)));
        return std::static_pointer_cast<T>(svc);
    }

    virtual ServiceContainer& getServiceContainer() = 0;

protected:
    virtual std::shared_ptr<void> getServiceByType(const std::type_index& type) const = 0;
};

} // namespace rex::modules
