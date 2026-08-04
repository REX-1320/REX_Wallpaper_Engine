#pragma once

#include <rex/wallpaper/IWallpaperEngine.hpp>

class Compositor;

namespace rex::wallpaper {

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual bool load(const SceneInfo& scene, Compositor& compositor) = 0;
    virtual void update(double deltaTime) = 0;
    virtual void render(Compositor& compositor) = 0;
    virtual void unload() = 0;

    virtual void play() {}
    virtual void pause() {}
    virtual void stop() {}
    virtual bool isPlaying() const { return false; }
    virtual double getDuration() const { return 0.0; }
    virtual double getWidth() const { return 0.0; }
    virtual double getHeight() const { return 0.0; }
};

} // namespace rex::wallpaper
