#pragma once
#include "IRenderer.hpp"
#include <string>

namespace rex::wallpaper {

class ImageRenderer : public IRenderer {
public:
    bool load(const SceneInfo& scene, Compositor& compositor) override;
    void update(double) override {}
    void render(Compositor&) override {}
    void unload() override {}

private:
    std::string m_path;
};

} // namespace
