#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "ModuleRegistry.hpp"
#include "DependencyResolver.hpp"
#include "EventBus.hpp"
#include "ServiceContainer.hpp"
#include "ModuleState.hpp"

namespace rex::modules {

class IModule;

class LifecycleCoordinator {
public:
    LifecycleCoordinator(ModuleRegistry& registry, EventBus& bus,
                         ServiceContainer& container);

    void initializeAll(const std::vector<std::string>& startupOrder);
    void startModule(const std::string& id);
    void stopModule(const std::string& id);
    void pauseModule(const std::string& id);
    void resumeModule(const std::string& id);
    void restartModule(const std::string& id);
    void shutdownAll(const std::vector<std::string>& reverseOrder);

    void update(float dt);

    void buildDependencyGraph();

private:
    bool startSingle(IModule* mod);
    void stopSingle(IModule* mod);
    void cascadeStop(const std::string& id);
    void cascadePause(const std::string& id);

    ModuleRegistry& registry_;
    EventBus& bus_;
    ServiceContainer& container_;
    std::unordered_map<std::string, std::vector<std::string>> dependents_;
    std::mutex mutex_;
};

} // namespace rex::modules
