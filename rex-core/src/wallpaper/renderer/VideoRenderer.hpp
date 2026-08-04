#pragma once
#include "IRenderer.hpp"
#include "wallpaper/playback/PlaybackController.hpp"
#include <memory>
#include <string>

namespace rex::wallpaper {

class VideoRenderer : public IRenderer {
public:
    bool load(const SceneInfo& scene, Compositor& compositor) override;
    void update(double deltaTime) override;
    void render(Compositor& compositor) override;
    void unload() override;

    void play() override;
    void pause() override;
    void stop() override;
    bool isPlaying() const override;
    double getDuration() const override;
    double getWidth() const override;
    double getHeight() const override;

private:
    std::unique_ptr<PlaybackController> m_playback;
    static constexpr uint64_t VIDEO_LAYER_ID = 1;
};

} // namespace
