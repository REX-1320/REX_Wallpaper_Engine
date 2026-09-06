#include "ImageRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <shared/Logger.hpp>

namespace rex::wallpaper {

bool ImageRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_path = scene.sourcePath;
    if (m_path.empty()) {
        REX_LOG_WARN("ImageRenderer", "Empty path");
        return false;
    }
    REX_LOG_INFO("ImageRenderer", "Loading: {}", m_path);
    return compositor.SetBackgroundFromFile(m_path.c_str());
}

} // namespace
