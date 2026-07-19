#pragma once

#include "wallpaper/decoder/WICTextureLoader.hpp"

#include <shared/Result.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::graphics {

// ─── TextureCache ───────────────────────────────────────────────────────────
//
// CPU-side pixel cache for decoded wallpaper images.
//
// Caches decoded RGBA8 pixel data keyed by file path. The cache does NOT
// own GPU resources — the renderer is responsible for uploading cached
// pixels to the GPU. This separation means:
//
//   - The cache can be shared across renderer instances.
//   - Hot-reload reloads from disk and updates the cache; the renderer
//     notices the version change and re-uploads to GPU.
//   - No D3D12 device dependency in the cache.
//
// ## Thread safety
//
// All public methods are protected by a mutex. The cache can be accessed
// from multiple threads (e.g., a hot-reload watcher thread and the
// render thread) without external synchronization.
//
// ## Cache entries
//
// Each entry stores:
//   - decoded pixel data (RGBA8, as returned by DecodeTextureFromFile)
//   - file write time (for hot-reload invalidation)
//   - a version counter (incremented on each reload)

struct CachedTexture {
    wallpaper::TextureData pixels;
    int64_t writeTime = 0;     // file modification time
    uint64_t version = 0;      // incremented on each successful reload
    bool valid = false;
};

class TextureCache {
public:
    TextureCache() = default;
    ~TextureCache() = default;

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // ── Cache operations ────────────────────────────────────────────────

    /// Load an image into the cache. Returns the entry on success.
    /// If the file has already been cached and hasn't changed, returns
    /// the existing entry without re-decoding.
    shared::Result<const CachedTexture*> GetOrLoad(std::string_view path);

    /// Reload a cached entry from disk (for hot-reload). Returns the
    /// updated entry, or the existing entry if the file didn't change.
    shared::Result<const CachedTexture*> Reload(std::string_view path);

    /// Evict an entry from the cache.
    void Evict(std::string_view path);

    /// Remove all cached entries.
    void Clear();

    /// Check whether an entry is cached.
    bool Contains(std::string_view path) const;

    /// Get a pointer to a cached entry, or nullptr.
    const CachedTexture* Get(std::string_view path) const;

    /// Number of cached entries.
    size_t Size() const;

    // ── Hot-reload polling ──────────────────────────────────────────────

    /// Check all cached entries for file changes. Returns a list of
    /// paths whose files have been modified since they were cached.
    std::vector<std::string> PollChangedFiles();

private:
    /// Decode from disk and update cache entry.
    shared::Result<void> LoadFromDisk(std::string_view path);

    /// Get file write time, or 0 on failure.
    static int64_t GetFileWriteTime(std::string_view path);

    struct Entry {
        CachedTexture texture;
        std::string key; // stable string for pointer stability
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace rex::graphics
