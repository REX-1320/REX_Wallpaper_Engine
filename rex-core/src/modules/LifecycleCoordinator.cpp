#include "LifecycleCoordinator.hpp"
#include "interfaces/IModule.hpp"
#include "ModuleContext.hpp"
#include <stdexcept>
#include <iostream>

namespace rex::modules {

LifecycleCoordinator::LifecycleCoordinator(ModuleRegistry& registry,
                                           EventBus& bus,
                                           ServiceContainer& container)
    : registry_(registry), bus_(bus), container_(container) {}

void LifecycleCoordinator::buildDependencyGraph() {
    std::lock_guard<std::mutex> lock(mutex_);
    dependents_.clear();
    for (auto* mod : registry_.getAllModules()) {
        const auto& meta = mod->getMetadata();
        for (const auto& depId : meta.dependencies) {
            dependents_[depId].push_back(meta.id);
        }
    }
}

void LifecycleCoordinator::initializeAll(const std::vector<std::string>& startupOrder) {
    for (const auto& id : startupOrder) {
        auto* mod = registry_.getModule(id);
        if (!mod) continue;

        auto state = registry_.getModuleState(id);
        if (state != ModuleState::Registered && state != ModuleState::Unknown)
            continue;

        registry_.setModuleState(id, ModuleState::Initializing);
        bus_.publish({ModuleEventType::ModuleInitialized, id, state, ModuleState::Initializing});

        ModuleContext ctx(id, container_, registry_, bus_, "config/" + id + ".json");

        bool success = false;
        try {
            success = mod->initialize(&ctx);
        } catch (const std::exception& e) {
            registry_.setModuleState(id, ModuleState::Failed);
            bus_.publish({ModuleEventType::ModuleFailed, id, ModuleState::Initializing, ModuleState::Failed, e.what()});
            continue;
        }

        if (success) {
            registry_.setModuleState(id, ModuleState::Initialized);
            bus_.publish({ModuleEventType::ModuleInitialized, id, ModuleState::Initializing, ModuleState::Initialized});
        } else {
            registry_.setModuleState(id, ModuleState::Failed);
            bus_.publish({ModuleEventType::ModuleFailed, id, ModuleState::Initializing, ModuleState::Failed, "initialize() returned false"});
        }
    }
}

void LifecycleCoordinator::startModule(const std::string& id) {
    auto* mod = registry_.getModule(id);
    if (!mod) return;
    startSingle(mod);
}

bool LifecycleCoordinator::startSingle(IModule* mod) {
    const auto& id = mod->getMetadata().id;
    auto state = registry_.getModuleState(id);
    if (state != ModuleState::Initialized && state != ModuleState::Stopped)
        return false;

    for (const auto& depId : mod->getMetadata().dependencies) {
        auto* dep = registry_.getModule(depId);
        if (dep && registry_.getModuleState(depId) != ModuleState::Running) {
            registry_.setModuleState(id, ModuleState::Failed);
            bus_.publish({ModuleEventType::ModuleFailed, id, state, ModuleState::Failed, "Dependency not running: " + depId});
            return false;
        }
    }

    registry_.setModuleState(id, ModuleState::Starting);

    bool success = false;
    try {
        success = mod->start();
    } catch (const std::exception& e) {
        registry_.setModuleState(id, ModuleState::Failed);
        bus_.publish({ModuleEventType::ModuleFailed, id, ModuleState::Starting, ModuleState::Failed, e.what()});
        return false;
    }

    if (success) {
        registry_.setModuleState(id, ModuleState::Running);
        bus_.publish({ModuleEventType::ModuleStarted, id, ModuleState::Starting, ModuleState::Running});
    } else {
        registry_.setModuleState(id, ModuleState::Failed);
        bus_.publish({ModuleEventType::ModuleFailed, id, ModuleState::Starting, ModuleState::Failed, "start() returned false"});
    }
    return success;
}

void LifecycleCoordinator::stopModule(const std::string& id) {
    auto* mod = registry_.getModule(id);
    if (!mod) return;
    stopSingle(mod);
}

void LifecycleCoordinator::stopSingle(IModule* mod) {
    const auto& id = mod->getMetadata().id;
    auto state = registry_.getModuleState(id);
    if (state != ModuleState::Running && state != ModuleState::Paused)
        return;

    registry_.setModuleState(id, ModuleState::Stopping);
    bus_.publish({ModuleEventType::ModuleStopped, id, state, ModuleState::Stopping});

    try {
        mod->stop();
    } catch (const std::exception& e) {
        std::cerr << "[Lifecycle] stop() threw for " << id << ": " << e.what() << "\n";
    }

    registry_.setModuleState(id, ModuleState::Stopped);
    bus_.publish({ModuleEventType::ModuleStopped, id, ModuleState::Stopping, ModuleState::Stopped});

    cascadeStop(id);
}

void LifecycleCoordinator::pauseModule(const std::string& id) {
    auto* mod = registry_.getModule(id);
    if (!mod) return;
    auto state = registry_.getModuleState(id);
    if (state != ModuleState::Running) return;

    try {
        if (!mod->pause()) return;
    } catch (...) { return; }

    registry_.setModuleState(id, ModuleState::Paused);
    bus_.publish({ModuleEventType::ModulePaused, id, state, ModuleState::Paused});
    cascadePause(id);
}

void LifecycleCoordinator::resumeModule(const std::string& id) {
    auto* mod = registry_.getModule(id);
    if (!mod) return;
    auto state = registry_.getModuleState(id);
    if (state != ModuleState::Paused) return;

    try {
        if (!mod->resume()) return;
    } catch (...) { return; }

    registry_.setModuleState(id, ModuleState::Running);
    bus_.publish({ModuleEventType::ModuleResumed, id, state, ModuleState::Running});
}

void LifecycleCoordinator::restartModule(const std::string& id) {
    auto* mod = registry_.getModule(id);
    if (!mod) return;

    stopSingle(mod);
    startSingle(mod);
}

void LifecycleCoordinator::shutdownAll(const std::vector<std::string>& reverseOrder) {
    for (const auto& id : reverseOrder) {
        auto* mod = registry_.getModule(id);
        if (!mod) continue;

        auto state = registry_.getModuleState(id);
        if (state == ModuleState::Running || state == ModuleState::Paused) {
            stopSingle(mod);
        }

        registry_.setModuleState(id, ModuleState::Unloading);
        bus_.publish({ModuleEventType::ModuleUnloaded, id, state, ModuleState::Unloading});

        try {
            mod->shutdown();
        } catch (...) {}

        registry_.setModuleState(id, ModuleState::Unloaded);
        bus_.publish({ModuleEventType::ModuleUnloaded, id, ModuleState::Unloading, ModuleState::Unloaded});
    }
}

void LifecycleCoordinator::update(float dt) {
    for (auto* mod : registry_.getAllModules()) {
        auto state = registry_.getModuleState(mod->getMetadata().id);
        if (state == ModuleState::Running || state == ModuleState::Paused) {
            try {
                mod->update(dt);
            } catch (...) {}
        }
    }
}

void LifecycleCoordinator::cascadeStop(const std::string& id) {
    auto it = dependents_.find(id);
    if (it == dependents_.end()) return;

    for (const auto& depId : it->second) {
        auto* depMod = registry_.getModule(depId);
        if (!depMod) continue;
        auto depState = registry_.getModuleState(depId);
        if (depState == ModuleState::Running || depState == ModuleState::Paused) {
            stopSingle(depMod);
            cascadeStop(depId);
        }
    }
}

void LifecycleCoordinator::cascadePause(const std::string& id) {
    auto it = dependents_.find(id);
    if (it == dependents_.end()) return;

    for (const auto& depId : it->second) {
        auto* depMod = registry_.getModule(depId);
        if (!depMod) continue;
        auto depState = registry_.getModuleState(depId);
        if (depState == ModuleState::Running) {
            try {
                if (depMod->pause()) {
                    registry_.setModuleState(depId, ModuleState::Paused);
                    bus_.publish({ModuleEventType::ModulePaused, depId, depState, ModuleState::Paused});
                    cascadePause(depId);
                }
            } catch (...) {}
        }
    }
}

} // namespace rex::modules
