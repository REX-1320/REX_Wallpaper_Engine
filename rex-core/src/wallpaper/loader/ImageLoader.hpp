#pragma once

#include "graphics/resource/TextureCache.hpp"

#include <shared/Result.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::wallpaper::loader {

// ─── SupportedFormats ──────────────────────────────────────────────────────
//
// Image formats decodable by WIC (Windows Imaging Component).
// Codec availability depends on the Windows version and installed codecs.

enum class ImageFormat {
    Unknown,
    PNG,
    JPEG,
    BMP,
    TIFF,
    WEBP,  // requires Windows 10 1809+
    GIF,
    ICO,
};

// ─── ImageLoader ────────────────────────────────────────────────────────────
//
// Loads and caches wallpaper images. Provides:
//
//   - Async loading with callback
//   - Format detection / validation
//   - Hot-reload via periodic polling (file modification time)
//   - Texture cache integration
//
// ## Hot-reload
//
// Call StartHotReload() with a polling interval. The loader will check
// all cached images for file changes. When a change is detected, the
// image is reloaded and the changeCallback is invoked.
//
// ## Thread safety
//
// The underlying TextureCache is thread-safe. ImageLoader itself must
// be used from a single thread for Start/Stop/Reload control, but
// GetOrLoad can be called from any thread.

class ImageLoader {
public:
    /// Called when a hot-reload detects a changed file.
    /// The path is the file that changed.
    using ChangeCallback = std::function<void(std::string_view path)>;

    ImageLoader();
    ~ImageLoader();

    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // ── Loading ─────────────────────────────────────────────────────────

    /// Load an image, caching if successful. Returns the decoded pixel
    /// data. Supported formats: PNG, JPEG, BMP, TIFF, WEBP, GIF, ICO.
    shared::Result<const graphics::CachedTexture*> Load(
        std::string_view path);

    /// Reload an image from disk (for hot-reload or explicit refresh).
    shared::Result<const graphics::CachedTexture*> Reload(
        std::string_view path);

    /// Evict an image from the cache.
    void Unload(std::string_view path);

    /// Evict all cached images.
    void Clear();

    // ── Queries ─────────────────────────────────────────────────────────

    /// Check whether an image is cached.
    bool IsLoaded(std::string_view path) const;

    /// Get cached texture data, or nullptr.
    const graphics::CachedTexture* Get(std::string_view path) const;

    /// Detect the image format from file extension.
    static ImageFormat DetectFormat(std::string_view path);

    /// Whether the format is supported.
    static bool IsFormatSupported(std::string_view path);

    /// Number of cached images.
    size_t CacheSize() const;

    // ── Hot-reload ──────────────────────────────────────────────────────

    /// Start hot-reload polling. Checks all cached files for changes
    /// every `intervalMs` milliseconds. When a change is detected,
    /// reloads the file and invokes `onChange`.
    void StartHotReload(std::chrono::milliseconds intervalMs,
                        ChangeCallback onChange);

    /// Stop hot-reload polling.
    void StopHotReload();

    /// Manually poll for changed files. Reloads changed files and
    /// returns their paths. Does not invoke the change callback.
    std::vector<std::string> PollChanges();

private:
    graphics::TextureCache m_cache;

    // Hot-reload
    std::atomic<bool> m_hotReloadRunning{false};
    std::thread m_hotReloadThread;
    ChangeCallback m_changeCallback;
    std::chrono::milliseconds m_pollInterval{2000};
};

} // namespace rex::wallpaper::loader
