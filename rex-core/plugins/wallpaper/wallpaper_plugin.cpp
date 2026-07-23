#include <cstring>
#include <iostream>
#include "modules/interfaces/IModule.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "modules/ModuleSignature.hpp"

using namespace rex::modules;

class WallpaperPlugin : public IModule {
public:
    WallpaperPlugin() {
        meta_.id = "rex.wallpaper";
        meta_.name = "Wallpaper";
        meta_.version = "1.0.0";
        meta_.author = "REX Team";
        meta_.description = "Animated wallpaper renderer";
        meta_.category = "Desktop";
        meta_.builtIn = false;
        meta_.autoStart = false;
        meta_.dependencies = {"rex.graphics"};
    }

    bool initialize(IModuleContext* ctx) override {
        std::cout << "[WallpaperPlugin] init\n";
        return true;
    }

    bool start() override {
        std::cout << "[WallpaperPlugin] start\n";
        return true;
    }

    void stop() override {
        std::cout << "[WallpaperPlugin] stop\n";
    }

    void shutdown() override {
        std::cout << "[WallpaperPlugin] shutdown\n";
    }

    const ModuleMetadata& getMetadata() const override { return meta_; }
    ModuleState getState() const override { return ModuleState::Running; }

private:
    ModuleMetadata meta_;
};

extern "C" {

const ModuleSignature* GetModuleSignature() {
    static ModuleSignature sig{"rex.wallpaper", "1.0.0", "1.0", "", 1};
    return &sig;
}

IModule* CreateModule() {
    return new WallpaperPlugin();
}

}
