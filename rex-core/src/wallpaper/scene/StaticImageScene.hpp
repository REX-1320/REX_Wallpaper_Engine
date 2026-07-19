#pragma once

#include "graphics/resource/TextureCache.hpp"
#include "wallpaper/loader/ImageLoader.hpp"

#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <shared/Result.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace rex::wallpaper::scene {

// ─── StaticImageScene ───────────────────────────────────────────────────────
//
// Manages a single static image wallpaper.
//
// Owns the scene lifecycle:
//   1. Load(imagePath)  — decode image, upload to GPU
//   2. ApplyTo(renderer) — configure the renderer's texture + fill mode
//   3. Unload()         — release GPU texture, evict from cache
//
// ## Fill modes
//
//   Fill    — scale to cover viewport, crop excess (default)
//   Fit     — scale to fit within viewport, letterbox
//   Stretch — stretch to fill viewport, ignore aspect ratio
//   Center  — center at native resolution, no scaling
//
// ## Hot-reload
//
// When the image file is modified on disk, call Reload() to re-decode
// and re-upload to GPU. The renderer will pick up the new texture on
// the next frame.
//
// ## Thread safety
//
// Load/Unload/Reload must be called from the engine's render thread.
// The underlying ImageLoader is thread-safe for concurrent loads.

class StaticImageScene {
public:
    StaticImageScene(std::shared_ptr<loader::ImageLoader> imageLoader);
    ~StaticImageScene();

    StaticImageScene(const StaticImageScene&) = delete;
    StaticImageScene& operator=(const StaticImageScene&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// Load the image at `path`. Decodes via WIC and caches pixel data.
    shared::Result<void> Load(std::string_view path);

    /// Reload from disk (hot-reload).
    shared::Result<void> Reload();

    /// Release GPU resources.
    void Unload();

    // ── State ──────────────────────────────────────────────────────────

    bool IsLoaded() const { return m_loaded; }
    std::string_view GetPath() const { return m_path; }
    int GetImageWidth()  const { return m_width; }
    int GetImageHeight() const { return m_height; }
    FillMode GetFillMode() const { return m_fillMode; }

    // ── Settings ───────────────────────────────────────────────────────

    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    void SetBrightness(float value) { m_brightness = value; }
    void SetContrast(float value)   { m_contrast = value; }
    void SetSaturation(float value) { m_saturation = value; }

    float GetBrightness() const { return m_brightness; }
    float GetContrast()   const { return m_contrast; }
    float GetSaturation() const { return m_saturation; }

    // ── Pixel data ─────────────────────────────────────────────────────

    /// Raw pixel data for GPU upload. Valid after Load().
    const uint8_t* GetPixels()   const;
    int            GetStride()   const;
    int            GetRowPitch() const;

    /// Version counter, incremented on each reload.
    /// The renderer can track this to detect when re-upload is needed.
    uint64_t GetVersion() const { return m_version; }

private:
    std::shared_ptr<loader::ImageLoader> m_imageLoader;
    std::string m_path;
    bool m_loaded = false;

    int m_width  = 0;
    int m_height = 0;

    FillMode m_fillMode    = FillMode::Fill;
    float m_brightness     = 1.0f;
    float m_contrast       = 1.0f;
    float m_saturation     = 1.0f;

    // Pixel cache pointer (owned by TextureCache)
    const graphics::CachedTexture* m_texture = nullptr;
    uint64_t m_version = 0;
};

} // namespace rex::wallpaper::scene
