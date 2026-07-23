#pragma once
#include <memory>
#include <string>
#include "interfaces/IModule.hpp"

namespace rex::modules {

class IModuleContext;

class PluginSandbox {
public:
    virtual ~PluginSandbox() = default;

    virtual std::unique_ptr<IModule> createModule(const std::string& dllPath, IModuleContext* context) = 0;
    virtual void shutdown() = 0;
    virtual std::unique_ptr<IModule> reloadModule(const std::string& dllPath, IModuleContext* context) = 0;
    virtual bool isOutOfProcess() const { return false; }
};

class InProcessSandbox : public PluginSandbox {
public:
    std::unique_ptr<IModule> createModule(const std::string& dllPath, IModuleContext* context) override;
    void shutdown() override;
    std::unique_ptr<IModule> reloadModule(const std::string& dllPath, IModuleContext* context) override;

private:
    void* libraryHandle_ = nullptr;
};

} // namespace rex::modules
