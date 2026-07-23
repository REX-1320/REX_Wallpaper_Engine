#pragma once

#include <memory>
#include <string>
#include <vector>
#include <typeindex>
#include "interfaces/IModule.hpp"
#include "interfaces/IModuleContext.hpp"
#include "ModuleRegistry.hpp"
#include "ModuleLoader.hpp"
#include "ModuleState.hpp"
#include "ModuleEvents.hpp"
#include "ModuleHealth.hpp"
#include "HealthMonitor.hpp"
#include "ConfigurationStore.hpp"
#include "PluginManifest.hpp"

namespace rex::modules {

class ModuleOrchestrator;
class UpdateManager;

class ModuleManager : public IModuleContext {
public:
    ModuleManager();
    ~ModuleManager() override;

    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;
    ModuleManager(ModuleManager&&) = delete;
    ModuleManager& operator=(ModuleManager&&) = delete;

    bool initialize();
    bool startAll();
    void stopAll();
    void shutdown();

    void update(float deltaTime);

    std::string getConfigPath() const override;

    void subscribeToEvents(EventCallback callback) override;
    void postEvent(const ModuleEvent& event) override;

    ModuleLoader& getLoader();
    ModuleRegistry& getRegistry();
    const HealthMonitor& getHealthMonitor() const;
    ConfigurationStore& getConfigStore();

    bool startModule(const std::string& id);
    bool stopModule(const std::string& id);
    bool pauseModule(const std::string& id);
    bool resumeModule(const std::string& id);
    bool restartModule(const std::string& id);
    void enableModule(const std::string& id);
    void disableModule(const std::string& id);

    bool loadDynamicModule(const std::string& dllPath);
    void reloadModule(const std::string& id);

    ModuleState getModuleState(const std::string& id) const;
    ModuleHealth getModuleHealth(const std::string& id) const;

    void updateHealth();

    void setUpdateManager(std::unique_ptr<UpdateManager> updateMgr);
    UpdateManager* getUpdateManager() const;

    std::vector<PluginManifest> checkForUpdates();
    bool installModuleFromMarketplace(const std::string& moduleId);

    ModuleOrchestrator& getOrchestrator() { return *orchestrator_; }

protected:
    std::shared_ptr<void> getServiceByType(const std::type_index& type) const override;

private:
    std::unique_ptr<ModuleOrchestrator> orchestrator_;
};

} // namespace rex::modules
