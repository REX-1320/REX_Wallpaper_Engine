#include "PluginSandbox.hpp"
#include "ModuleSignature.hpp"
#include <stdexcept>
#include <iostream>

#ifdef _WIN32
    #include <windows.h>
    #define LOAD_LIB(path)   LoadLibraryA(path.c_str())
    #define GET_SYM(handle, name) GetProcAddress((HMODULE)handle, name)
    #define FREE_LIB(handle) FreeLibrary((HMODULE)handle)
#else
    #include <dlfcn.h>
    #define LOAD_LIB(path)   dlopen(path.c_str(), RTLD_NOW)
    #define GET_SYM(handle, name) dlsym(handle, name)
    #define FREE_LIB(handle) dlclose(handle)
#endif

namespace rex::modules {

std::unique_ptr<IModule> InProcessSandbox::createModule(const std::string& dllPath, IModuleContext* context) {
    void* handle = LOAD_LIB(dllPath);
    if (!handle) {
        std::cerr << "[Sandbox] Failed to load: " << dllPath << "\n";
        return nullptr;
    }
    libraryHandle_ = handle;

    auto getSig = reinterpret_cast<GetModuleSignatureFn>(GET_SYM(handle, "GetModuleSignature"));
    if (getSig) {
        const ModuleSignature* sig = getSig();
        if (!sig) {
            std::cerr << "[Sandbox] No signature returned.\n";
            FREE_LIB(handle);
            libraryHandle_ = nullptr;
            return nullptr;
        }
        if (sig->apiVersion != 1) {
            std::cerr << "[Sandbox] Incompatible API version: " << sig->apiVersion << "\n";
            FREE_LIB(handle);
            libraryHandle_ = nullptr;
            return nullptr;
        }
    }

    using FactoryFn = IModule* (*)();
    auto factory = reinterpret_cast<FactoryFn>(GET_SYM(handle, "CreateModule"));
    if (!factory) {
        std::cerr << "[Sandbox] Symbol 'CreateModule' not found.\n";
        FREE_LIB(handle);
        libraryHandle_ = nullptr;
        return nullptr;
    }

    IModule* raw = factory();
    if (!raw) {
        FREE_LIB(handle);
        libraryHandle_ = nullptr;
        return nullptr;
    }

    return std::unique_ptr<IModule>(raw);
}

void InProcessSandbox::shutdown() {
    if (libraryHandle_) {
        FREE_LIB(libraryHandle_);
        libraryHandle_ = nullptr;
    }
}

std::unique_ptr<IModule> InProcessSandbox::reloadModule(const std::string& dllPath, IModuleContext* context) {
    shutdown();
    return createModule(dllPath, context);
}

} // namespace rex::modules
