#include "ModuleLoader.hpp"
#include "ModuleSignature.hpp"
#include "interfaces/IModuleContext.hpp"
#include <iostream>

namespace rex::modules {

void ModuleLoader::registerBuiltIn(const std::string& id, FactoryFunc factory) {
    builtinFactories_[id] = factory;
}

std::vector<std::unique_ptr<IModule>> ModuleLoader::loadAllBuiltIns() {
    std::vector<std::unique_ptr<IModule>> modules;
    for (auto& [id, factory] : builtinFactories_) {
        auto mod = factory();
        if (mod) modules.push_back(std::move(mod));
    }
    return modules;
}

std::unique_ptr<IModule> ModuleLoader::loadDynamic(const std::string& dllPath, IModuleContext* context) {
    auto sandbox = std::make_unique<InProcessSandbox>();
    auto module = sandbox->createModule(dllPath, context);
    if (!module) return nullptr;

    const std::string& id = module->getMetadata().id;
    dynamicSandboxes_[id] = std::move(sandbox);
    moduleSandboxMap_[id] = dynamicSandboxes_[id].get();
    dynamicPaths_[id] = dllPath;
    return module;
}

void ModuleLoader::unloadDynamic(const std::string& moduleId) {
    auto it = dynamicSandboxes_.find(moduleId);
    if (it != dynamicSandboxes_.end()) {
        it->second->shutdown();
        dynamicSandboxes_.erase(it);
        moduleSandboxMap_.erase(moduleId);
    }
}

std::unique_ptr<IModule> ModuleLoader::reloadDynamic(const std::string& moduleId, const std::string& dllPath, IModuleContext* context) {
    auto it = dynamicSandboxes_.find(moduleId);
    if (it == dynamicSandboxes_.end()) {
        return loadDynamic(dllPath, context);
    }

    auto newModule = it->second->reloadModule(dllPath, context);
    if (!newModule) {
        dynamicSandboxes_.erase(it);
        moduleSandboxMap_.erase(moduleId);
        return nullptr;
    }
    dynamicPaths_[moduleId] = dllPath;
    return newModule;
}

bool ModuleLoader::isDynamic(const std::string& moduleId) const {
    return moduleSandboxMap_.find(moduleId) != moduleSandboxMap_.end();
}

void ModuleLoader::setDynamicPath(const std::string& moduleId, const std::string& path) {
    dynamicPaths_[moduleId] = path;
}

std::string ModuleLoader::getDynamicPath(const std::string& moduleId) const {
    auto it = dynamicPaths_.find(moduleId);
    return (it != dynamicPaths_.end()) ? it->second : "";
}

} // namespace rex::modules
