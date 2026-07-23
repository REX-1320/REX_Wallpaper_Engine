#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include "interfaces/IModule.hpp"
#include "PluginSandbox.hpp"

namespace rex::modules {

class IModuleContext;

class ModuleLoader {
public:
    using FactoryFunc = std::unique_ptr<IModule>(*)();

    void registerBuiltIn(const std::string& id, FactoryFunc factory);
    std::vector<std::unique_ptr<IModule>> loadAllBuiltIns();

    std::unique_ptr<IModule> loadDynamic(const std::string& dllPath, IModuleContext* context);
    void unloadDynamic(const std::string& moduleId);
    std::unique_ptr<IModule> reloadDynamic(const std::string& moduleId, const std::string& dllPath, IModuleContext* context);
    bool isDynamic(const std::string& moduleId) const;

    void setDynamicPath(const std::string& moduleId, const std::string& path);
    std::string getDynamicPath(const std::string& moduleId) const;

private:
    std::unordered_map<std::string, FactoryFunc> builtinFactories_;
    std::unordered_map<std::string, std::unique_ptr<PluginSandbox>> dynamicSandboxes_;
    std::unordered_map<std::string, PluginSandbox*> moduleSandboxMap_;
    std::unordered_map<std::string, std::string> dynamicPaths_;
};

} // namespace rex::modules
