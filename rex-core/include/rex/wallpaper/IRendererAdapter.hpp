#pragma once

#include "rex/wallpaper/IWallpaperEngine.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

// ─── IRendererAdapter ──────────────────────────────────────────────────────
//
// Bridges the generic engine property surface to a concrete renderer.
// Each renderer declares the properties it can actually apply, so callers
// receive an explicit "unsupported" error instead of a silent no-op.
//
//   LoadScene()   → creates the adapter for the scene's type
//   SetProperty() → rejects unknown / unsupported properties
//   GetSupportedProperties() → introspection for UI gating

class IRendererAdapter {
public:
    virtual ~IRendererAdapter() = default;

    /// Renderer type name: "static", "video", "web", "shader", "scene",
    /// "audio_visualizer", or "unsupported".
    virtual std::string_view GetType() const = 0;

    /// Properties this renderer can apply (see Engine::SetProperty).
    virtual std::vector<std::string> GetSupportedProperties() const = 0;

    virtual bool Supports(std::string_view property) const = 0;
};

// ─── Factory & Helpers ─────────────────────────────────────────────────────

std::unique_ptr<IRendererAdapter> CreateRendererAdapter(std::string_view sceneType);

std::optional<FillMode> ParseFillMode(std::string_view value);
std::string_view ToString(FillMode mode);

} // namespace rex::wallpaper
