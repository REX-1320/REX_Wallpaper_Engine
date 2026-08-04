#include "ImageRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>

namespace rex::wallpaper {

bool ImageRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_path = scene.sourcePath;
    if (m_path.empty()) {
        printf("[ImageRenderer] Empty path\n");
        return false;
    }
    printf("[ImageRenderer] Loading: %s\n", m_path.c_str());
    return compositor.SetBackgroundFromFile(m_path.c_str());
}

} // namespace
