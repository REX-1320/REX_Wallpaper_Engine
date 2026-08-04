#pragma once
#include "IRenderer.hpp"
#include <memory>

namespace rex::wallpaper {

class RendererManager {
public:
    void setRenderer(std::unique_ptr<IRenderer> renderer);
    IRenderer* getRenderer() { return m_renderer.get(); }
    const IRenderer* getRenderer() const { return m_renderer.get(); }
    void clear();

    void update(double dt) { if (m_renderer) m_renderer->update(dt); }
    void render(::Compositor& comp) { if (m_renderer) m_renderer->render(comp); }
    void play() { if (m_renderer) m_renderer->play(); }
    void pause() { if (m_renderer) m_renderer->pause(); }
    void stop() { if (m_renderer) m_renderer->stop(); }

private:
    std::unique_ptr<IRenderer> m_renderer;
};

inline void RendererManager::setRenderer(std::unique_ptr<IRenderer> renderer) {
    if (m_renderer) m_renderer->unload();
    m_renderer = std::move(renderer);
}

inline void RendererManager::clear() {
    if (m_renderer) m_renderer->unload();
    m_renderer.reset();
}

} // namespace
