#pragma once
#include <memory>
#include <string>
#include <vector>
#include "ModuleRegistry.hpp"
#include "EventBus.hpp"
#include "ServiceContainer.hpp"
#include "LifecycleCoordinator.hpp"
#include "HealthMonitor.hpp"
#include "ConfigurationStore.hpp"
#include "ModuleLoader.hpp"
#include "PluginManifest.hpp"
#include "UpdateManager.hpp"

namespace rex::modules {

class ModuleOrchestrator {
public:
    ModuleOrchestrator();

    void registerBuiltIn(const std::string& id, ModuleLoader::FactoryFunc factory);
    bool loadDynamicModule(const std::string& dllPath);

    void initialize();
    void shutdown();
    void update(float dt);

    void startModule(const std::string& id);
    void stopModule(const std::string& id);
    void pauseModule(const std::string& id);
    void resumeModule(const std::string& id);
    void restartModule(const std::string& id);
    void enableModule(const std::string& id);
    void disableModule(const std::string& id);

    void reloadModule(const std::string& id);
    void updateHealth();

    ModuleRegistry& getRegistry() { return registry_; }
    EventBus& getEventBus() { return bus_; }
    ServiceContainer& getServiceContainer() { return services_; }
    HealthMonitor& getHealthMonitor() { return health_; }
    ConfigurationStore& getConfigStore() { return config_; }
    ModuleLoader& getLoader() { return loader_; }
    UpdateManager* getUpdateManager() const { return updateMgr_.get(); }

    void setUpdateManager(std::unique_ptr<UpdateManager> mgr);

    std::vector<PluginManifest> checkForUpdates();
    bool installModuleFromMarketplace(const std::string& moduleId);

    ModuleState getModuleState(const std::string& id) const;
    ModuleHealth getModuleHealth(const std::string& id) const;

private:
    void handleModuleFailure(const std::string& id, const std::string& error);

    ModuleRegistry registry_;
    EventBus bus_;
    ServiceContainer services_;
    HealthMonitor health_;
    ConfigurationStore config_;
    ModuleLoader loader_;
    std::unique_ptr<LifecycleCoordinator> coordinator_;
    std::unique_ptr<UpdateManager> updateMgr_;
    std::vector<std::string> startupOrder_;
    bool initialized_ = false;
};

} // namespace rex::modules
