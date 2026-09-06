#include <rex/wallpaper/IRendererAdapter.hpp>

#include <set>

namespace rex::wallpaper {

// ─── FillMode helpers ──────────────────────────────────────────────────────

std::optional<FillMode> ParseFillMode(std::string_view s) {
    if (s == "fill")    return FillMode::Fill;
    if (s == "fit")     return FillMode::Fit;
    if (s == "stretch") return FillMode::Stretch;
    if (s == "center")  return FillMode::Center;
    if (s == "tile")    return FillMode::Tile;
    return std::nullopt;
}

std::string_view ToString(FillMode mode) {
    switch (mode) {
        case FillMode::Fill:    return "fill";
        case FillMode::Fit:     return "fit";
        case FillMode::Stretch: return "stretch";
        case FillMode::Center:  return "center";
        case FillMode::Tile:    return "tile";
    }
    return "fill";
}

namespace {

class BaseAdapter : public IRendererAdapter {
public:
    std::vector<std::string> GetSupportedProperties() const override {
        std::vector<std::string> props;
        props.reserve(m_supported.size());
        for (const auto& p : m_supported) {
            props.emplace_back(p);
        }
        return props;
    }

    bool Supports(std::string_view property) const override {
        return m_supported.find(property) != m_supported.end();
    }

protected:
    std::set<std::string, std::less<>> m_supported;
};

// Static image: rendered via a GPU texture. Only the fill mode is currently
// applied; image effects (brightness, contrast, ...) have no D3D12 pipeline
// yet, so they are reported as unsupported rather than silently ignored.
class StaticImageAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "static"; }
    StaticImageAdapter() {
        m_supported.insert("fillMode");
        m_supported.insert("rotation");
        m_supported.insert("flipH");
        m_supported.insert("flipV");
    }
};

// Video: fill mode via the renderer, speed/loop via the playback controller.
// Volume/mute have no audio pipeline yet — reported unsupported.
class VideoAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "video"; }
    VideoAdapter() {
        m_supported.insert("fillMode");
        m_supported.insert("speed");
        m_supported.insert("loop");
        m_supported.insert("rotation");
        m_supported.insert("flipH");
        m_supported.insert("flipV");
        m_supported.insert("volume");
        m_supported.insert("mute");
        m_supported.insert("brightness");
        m_supported.insert("contrast");
        m_supported.insert("saturation");
    }
};

// Web / shader / scene / audio-visualizer renderers are not implemented in
// the engine yet. They accept no properties — every apply is rejected.
class UnsupportedAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "unsupported"; }
};

} // namespace

std::unique_ptr<IRendererAdapter> CreateRendererAdapter(std::string_view sceneType) {
    if (sceneType == "video") return std::make_unique<VideoAdapter>();
    if (sceneType == "static" || sceneType == "image") return std::make_unique<StaticImageAdapter>();
    return std::make_unique<UnsupportedAdapter>();
}

} // namespace rex::wallpaper
