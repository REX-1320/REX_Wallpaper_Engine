#include "ModuleManager.hpp"
#include "ModuleOrchestrator.hpp"
#include "UpdateManager.hpp"

namespace rex::modules {

ModuleManager::ModuleManager()
    : orchestrator_(std::make_unique<ModuleOrchestrator>()) {}

ModuleManager::~ModuleManager() = default;

bool ModuleManager::initialize() { orchestrator_->initialize(); return true; }

bool ModuleManager::startAll() {
    for (auto mod : orchestrator_->getRegistry().getAllModules()) {
        orchestrator_->startModule(mod->getMetadata().id);
    }
    return true;
}

void ModuleManager::stopAll() {
    for (auto mod : orchestrator_->getRegistry().getAllModules()) {
        orchestrator_->stopModule(mod->getMetadata().id);
    }
}

void ModuleManager::shutdown() { orchestrator_->shutdown(); }

void ModuleManager::update(float deltaTime) { orchestrator_->update(deltaTime); }

std::string ModuleManager::getConfigPath() const { return "./config/modules/"; }

void ModuleManager::subscribeToEvents(EventCallback callback) {
    orchestrator_->getEventBus().subscribe(std::move(callback));
}

void ModuleManager::postEvent(const ModuleEvent& event) {
    orchestrator_->getEventBus().publish(event);
}

ModuleLoader& ModuleManager::getLoader() { return orchestrator_->getLoader(); }
ModuleRegistry& ModuleManager::getRegistry() { return orchestrator_->getRegistry(); }
const HealthMonitor& ModuleManager::getHealthMonitor() const { return orchestrator_->getHealthMonitor(); }
ConfigurationStore& ModuleManager::getConfigStore() { return orchestrator_->getConfigStore(); }

bool ModuleManager::startModule(const std::string& id) {
    orchestrator_->startModule(id);
    return orchestrator_->getModuleState(id) == ModuleState::Running;
}

bool ModuleManager::stopModule(const std::string& id) {
    orchestrator_->stopModule(id);
    return orchestrator_->getModuleState(id) == ModuleState::Stopped;
}

bool ModuleManager::pauseModule(const std::string& id) {
    orchestrator_->pauseModule(id);
    return orchestrator_->getModuleState(id) == ModuleState::Paused;
}

bool ModuleManager::resumeModule(const std::string& id) {
    orchestrator_->resumeModule(id);
    return orchestrator_->getModuleState(id) == ModuleState::Running;
}

bool ModuleManager::restartModule(const std::string& id) {
    orchestrator_->restartModule(id);
    return true;
}

void ModuleManager::enableModule(const std::string& id) { orchestrator_->enableModule(id); }
void ModuleManager::disableModule(const std::string& id) { orchestrator_->disableModule(id); }

bool ModuleManager::loadDynamicModule(const std::string& dllPath) {
    return orchestrator_->loadDynamicModule(dllPath);
}

void ModuleManager::reloadModule(const std::string& id) { orchestrator_->reloadModule(id); }

ModuleState ModuleManager::getModuleState(const std::string& id) const {
    return orchestrator_->getModuleState(id);
}

ModuleHealth ModuleManager::getModuleHealth(const std::string& id) const {
    return orchestrator_->getModuleHealth(id);
}

void ModuleManager::updateHealth() { orchestrator_->updateHealth(); }

void ModuleManager::setUpdateManager(std::unique_ptr<UpdateManager> updateMgr) {
    orchestrator_->setUpdateManager(std::move(updateMgr));
}

UpdateManager* ModuleManager::getUpdateManager() const {
    return orchestrator_->getUpdateManager();
}

std::vector<PluginManifest> ModuleManager::checkForUpdates() {
    return orchestrator_->checkForUpdates();
}

bool ModuleManager::installModuleFromMarketplace(const std::string& moduleId) {
    return orchestrator_->installModuleFromMarketplace(moduleId);
}

std::shared_ptr<void> ModuleManager::getServiceByType(const std::type_index& type) const {
    return orchestrator_->getServiceContainer().findService(type);
}

} // namespace rex::modules
