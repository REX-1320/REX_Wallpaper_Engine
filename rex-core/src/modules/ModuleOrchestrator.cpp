#include "ModuleOrchestrator.hpp"
#include "UpdateManager.hpp"
#include "DependencyResolver.hpp"
#include "ModuleContext.hpp"
#include "ModuleSignature.hpp"
#include <iostream>
#include <algorithm>

namespace rex::modules {

ModuleOrchestrator::ModuleOrchestrator()
    : config_("config/modules.json") {}

void ModuleOrchestrator::registerBuiltIn(const std::string& id, ModuleLoader::FactoryFunc factory) {
    loader_.registerBuiltIn(id, factory);
}

bool ModuleOrchestrator::loadDynamicModule(const std::string& dllPath) {
    auto mod = loader_.loadDynamic(dllPath, nullptr);
    if (!mod) return false;
    registry_.registerModule(std::move(mod));
    return true;
}

void ModuleOrchestrator::initialize() {
    auto builtins = loader_.loadAllBuiltIns();
    for (auto& mod : builtins)
        registry_.registerModule(std::move(mod));

    DependencyResolver resolver;
    std::vector<DependencyResolver::ModuleInfo> infos;
    for (auto mod : registry_.getAllModules()) {
        const auto& meta = mod->getMetadata();
        infos.push_back({meta.id, meta.dependencies});
    }

    try {
        startupOrder_ = resolver.resolveOrder(infos);
    } catch (const std::exception& e) {
        std::cerr << "[Orchestrator] Dependency resolution failed: " << e.what() << "\n";
        return;
    }

    coordinator_ = std::make_unique<LifecycleCoordinator>(registry_, bus_, services_);
    coordinator_->buildDependencyGraph();
    coordinator_->initializeAll(startupOrder_);

    auto savedConfigs = config_.loadAll();
    for (const auto& cfg : savedConfigs) {
        auto mod = registry_.getModule(cfg.moduleId);
        if (!mod) continue;

        if (cfg.enabled && (mod->getMetadata().autoStart || cfg.autoStart)) {
            if (registry_.getModuleState(cfg.moduleId) == ModuleState::Initialized) {
                coordinator_->startModule(cfg.moduleId);
            }
        } else if (!cfg.enabled) {
            mod->disable();
            registry_.setModuleState(cfg.moduleId, ModuleState::Disabled);
        }
    }

    initialized_ = true;
}

void ModuleOrchestrator::shutdown() {
    if (!initialized_) return;

    if (coordinator_) {
        std::vector<std::string> revOrder(startupOrder_.rbegin(), startupOrder_.rend());
        coordinator_->shutdownAll(revOrder);
    }

    std::vector<ModuleConfig> configs;
    for (auto mod : registry_.getAllModules()) {
        const auto& id = mod->getMetadata().id;
        ModuleConfig cfg;
        cfg.moduleId = id;
        cfg.enabled = (registry_.getModuleState(id) != ModuleState::Disabled);
        cfg.autoStart = mod->getMetadata().autoStart;
        configs.push_back(cfg);
    }
    config_.saveAll(configs);
    initialized_ = false;
}

void ModuleOrchestrator::update(float dt) {
    if (coordinator_) coordinator_->update(dt);

    for (auto mod : registry_.getAllModules()) {
        auto h = mod->getHealth();
        h.state = registry_.getModuleState(mod->getMetadata().id);
        health_.reportHealth(mod->getMetadata().id, h);
    }
}

void ModuleOrchestrator::startModule(const std::string& id) {
    if (coordinator_) coordinator_->startModule(id);
}

void ModuleOrchestrator::stopModule(const std::string& id) {
    if (coordinator_) coordinator_->stopModule(id);
}

void ModuleOrchestrator::pauseModule(const std::string& id) {
    if (coordinator_) coordinator_->pauseModule(id);
}

void ModuleOrchestrator::resumeModule(const std::string& id) {
    if (coordinator_) coordinator_->resumeModule(id);
}

void ModuleOrchestrator::restartModule(const std::string& id) {
    if (coordinator_) coordinator_->restartModule(id);
}

void ModuleOrchestrator::enableModule(const std::string& id) {
    auto mod = registry_.getModule(id);
    if (mod) mod->enable();
    if (coordinator_) coordinator_->startModule(id);

    ModuleConfig cfg = config_.loadModule(id);
    cfg.enabled = true;
    config_.saveModule(cfg);
}

void ModuleOrchestrator::disableModule(const std::string& id) {
    if (coordinator_) {
        coordinator_->stopModule(id);
    }
    auto mod = registry_.getModule(id);
    if (mod) mod->disable();
    registry_.setModuleState(id, ModuleState::Disabled);

    ModuleConfig cfg = config_.loadModule(id);
    cfg.enabled = false;
    config_.saveModule(cfg);
}

void ModuleOrchestrator::reloadModule(const std::string& id) {
    auto mod = registry_.getModule(id);
    if (!mod) throw std::runtime_error("Module not found: " + id);
    if (!loader_.isDynamic(id))
        throw std::runtime_error("Module is not dynamic: " + id);

    auto state = registry_.getModuleState(id);
    if (state != ModuleState::Stopped && state != ModuleState::Failed)
        throw std::runtime_error("Module must be stopped before reload");

    std::string dllPath = loader_.getDynamicPath(id);
    if (dllPath.empty())
        throw std::runtime_error("No DLL path stored for " + id);

    registry_.unregisterModule(id);
    loader_.unloadDynamic(id);

    auto newMod = loader_.loadDynamic(dllPath, nullptr);
    if (!newMod)
        throw std::runtime_error("Hot reload failed for " + id);

    const auto& newId = newMod->getMetadata().id;
    registry_.registerModule(std::move(newMod));

    ModuleContext ctx(newId, services_, registry_, bus_, "config/" + newId + ".json");
    auto registeredMod = registry_.getModule(newId);
    if (registeredMod && registeredMod->initialize(&ctx)) {
        registry_.setModuleState(newId, ModuleState::Initialized);
    }
}

void ModuleOrchestrator::handleModuleFailure(const std::string& id, const std::string& error) {
    auto mod = registry_.getModule(id);
    if (!mod) return;

    ModuleHealth h = health_.getHealth(id);
    h.crashCount++;
    health_.reportHealth(id, h);

    const int maxRestarts = 3;
    if (h.restartCount < maxRestarts) {
        h.restartCount++;
        health_.reportHealth(id, h);

        if (loader_.isDynamic(id)) {
            std::cerr << "[Orchestrator] Crash recovery: hot reloading " << id << "\n";
            try {
                reloadModule(id);
                if (registry_.getModuleState(id) == ModuleState::Initialized) {
                    coordinator_->startModule(id);
                }
            } catch (const std::exception& e) {
                std::cerr << "[Orchestrator] Reload failed: " << e.what() << "\n";
            }
        } else {
            std::cerr << "[Orchestrator] Restarting " << id
                      << " (attempt " << h.restartCount << ")\n";
            coordinator_->restartModule(id);
        }
    } else {
        std::cerr << "[Orchestrator] Module " << id << " exceeded restart limit.\n";
    }
}

void ModuleOrchestrator::updateHealth() {
    for (auto mod : registry_.getAllModules()) {
        const auto& id = mod->getMetadata().id;
        ModuleHealth h = mod->getHealth();
        h.state = registry_.getModuleState(id);
        health_.reportHealth(id, h);
    }
}

void ModuleOrchestrator::setUpdateManager(std::unique_ptr<UpdateManager> mgr) {
    updateMgr_ = std::move(mgr);
}

std::vector<PluginManifest> ModuleOrchestrator::checkForUpdates() {
    if (updateMgr_) return updateMgr_->checkForUpdates();
    return {};
}

bool ModuleOrchestrator::installModuleFromMarketplace(const std::string& moduleId) {
    if (!updateMgr_) throw std::runtime_error("UpdateManager not set");
    return updateMgr_->installOrUpdate(moduleId);
}

ModuleState ModuleOrchestrator::getModuleState(const std::string& id) const {
    return registry_.getModuleState(id);
}

ModuleHealth ModuleOrchestrator::getModuleHealth(const std::string& id) const {
    return health_.getHealth(id);
}

} // namespace rex::modules
