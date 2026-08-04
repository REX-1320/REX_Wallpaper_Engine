#include "VideoRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>

namespace rex::wallpaper {

bool VideoRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    if (scene.sourcePath.empty()) {
        printf("[VideoRenderer] No path\n");
        return false;
    }

    m_playback = std::make_unique<PlaybackController>();
    auto result = m_playback->Open(scene.sourcePath);
    if (result.IsError()) {
        printf("[VideoRenderer] Open failed: %s\n",
               result.Error().message.c_str());
        m_playback.reset();
        return false;
    }

    Layer videoLayer;
    videoLayer.id = VIDEO_LAYER_ID;
    videoLayer.kind = 0;
    videoLayer.bounds[0] = 0.0f;
    videoLayer.bounds[1] = 0.0f;
    videoLayer.bounds[2] = compositor.GetViewportWidth();
    videoLayer.bounds[3] = compositor.GetViewportHeight();
    videoLayer.opacity = 1.0f;
    videoLayer.zOrder = 0;
    videoLayer.visible = true;
    compositor.SubmitLayer(videoLayer);

    printf("[VideoRenderer] Loaded: %dx%d\n",
           m_playback->GetWidth(), m_playback->GetHeight());
    return true;
}

void VideoRenderer::update(double /*deltaTime*/) {}

void VideoRenderer::render(Compositor& compositor) {
    if (!m_playback || !m_playback->IsPlaying()) return;

    auto frame = m_playback->Tick();
    if (frame.valid) {
        compositor.UpdateLayerPixels(VIDEO_LAYER_ID,
                                     frame.pixels.data(),
                                     frame.width, frame.height,
                                     frame.rowPitch);
    }
}

void VideoRenderer::unload() {
    if (m_playback) {
        m_playback->Close();
        m_playback.reset();
    }
}

void VideoRenderer::play()   { if (m_playback) m_playback->Play(); }
void VideoRenderer::pause()  { if (m_playback) m_playback->Pause(); }
void VideoRenderer::stop()   { if (m_playback) m_playback->Stop(); }
bool VideoRenderer::isPlaying() const { return m_playback && m_playback->IsPlaying(); }
double VideoRenderer::getDuration() const { return m_playback ? m_playback->GetDuration() : 0.0; }
double VideoRenderer::getWidth() const { return m_playback ? static_cast<double>(m_playback->GetWidth()) : 0.0; }
double VideoRenderer::getHeight() const { return m_playback ? static_cast<double>(m_playback->GetHeight()) : 0.0; }

} // namespace
