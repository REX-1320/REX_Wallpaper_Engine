#include "TextureCache.hpp"

#include <shared/Logger.hpp>

#include <windows.h>
#include <string>

namespace rex::graphics {

// ─── Helpers ────────────────────────────────────────────────────────────────

int64_t TextureCache::GetFileWriteTime(std::string_view path) {
    // Convert to wide string for Windows API
    if (path.empty()) return 0;
    int len = MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                  static_cast<int>(path.size()), nullptr, 0);
    if (len <= 0) return 0;
    std::wstring wpath(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(),
                        static_cast<int>(path.size()), &wpath[0], len);

    HANDLE hFile = CreateFileW(
        wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    FILETIME ftWrite;
    BOOL ok = GetFileTime(hFile, nullptr, nullptr, &ftWrite);
    CloseHandle(hFile);

    if (!ok) return 0;

    return (static_cast<int64_t>(ftWrite.dwHighDateTime) << 32)
         | ftWrite.dwLowDateTime;
}

// ─── LoadFromDisk ───────────────────────────────────────────────────────────

shared::Result<void> TextureCache::LoadFromDisk(std::string_view path) {
    std::string key(path);

    // Try decoding the file
    auto texData = wallpaper::DecodeTextureFromFile(path);
    if (texData.pixels.empty()) {
        return shared::Result<void>(
            shared::ErrorCode::FileReadError,
            "Failed to decode image: " + key);
    }

    int64_t writeTime = GetFileWriteTime(path);

    // G1: publish a NEW CachedTexture object and swap it in — readers that
    // already hold the previous snapshot keep a valid, immutable object
    // (the old code overwrote the live entry's pixel vector in place).
    auto texture = std::make_shared<CachedTexture>();
    texture->pixels    = std::move(texData);
    texture->writeTime = writeTime;

    auto& entry = m_entries[key];
    texture->version = entry.texture ? entry.texture->version + 1 : 1;
    texture->valid   = true;
    entry.key     = key;
    entry.texture = std::move(texture);

    REX_LOG_INFO("TextureCache",
        "Cached: {} ({}x{}, v{})",
        path, entry.texture->pixels.width, entry.texture->pixels.height,
        entry.texture->version);

    return shared::Result<void>();
}

// ─── GetOrLoad ──────────────────────────────────────────────────────────────

shared::Result<std::shared_ptr<const CachedTexture>> TextureCache::GetOrLoad(
    std::string_view path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string key(path);

    // Check if already cached
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.texture
        && it->second.texture->valid) {
        return std::shared_ptr<const CachedTexture>(it->second.texture);
    }

    // Load from disk
    auto loadResult = LoadFromDisk(path);
    if (loadResult.IsError()) {
        return shared::Result<std::shared_ptr<const CachedTexture>>(
            loadResult.Error().code, loadResult.Error().message);
    }

    return std::shared_ptr<const CachedTexture>(m_entries[key].texture);
}

// ─── Reload ─────────────────────────────────────────────────────────────────

shared::Result<std::shared_ptr<const CachedTexture>> TextureCache::Reload(
    std::string_view path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string key(path);

    // Check write time
    int64_t diskTime = GetFileWriteTime(path);
    auto it = m_entries.find(key);
    if (it != m_entries.end()
        && it->second.texture
        && it->second.texture->valid
        && it->second.texture->writeTime == diskTime
        && diskTime != 0)
    {
        // File hasn't changed
        return std::shared_ptr<const CachedTexture>(it->second.texture);
    }

    // Reload from disk
    auto loadResult = LoadFromDisk(path);
    if (loadResult.IsError()) {
        return shared::Result<std::shared_ptr<const CachedTexture>>(
            loadResult.Error().code, loadResult.Error().message);
    }

    REX_LOG_INFO("TextureCache", "Hot-reloaded: {}", path);
    return std::shared_ptr<const CachedTexture>(m_entries[key].texture);
}

// ─── Evict / Clear ──────────────────────────────────────────────────────────

void TextureCache::Evict(std::string_view path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(std::string(path));
}

void TextureCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

// ─── Queries ────────────────────────────────────────────────────────────────

bool TextureCache::Contains(std::string_view path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(std::string(path));
    return it != m_entries.end() && it->second.texture
           && it->second.texture->valid;
}

std::shared_ptr<const CachedTexture> TextureCache::Get(
    std::string_view path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(std::string(path));
    if (it != m_entries.end() && it->second.texture
        && it->second.texture->valid) {
        return std::shared_ptr<const CachedTexture>(it->second.texture);
    }
    return nullptr;
}

size_t TextureCache::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

// ─── PollChangedFiles ───────────────────────────────────────────────────────

std::vector<std::string> TextureCache::PollChangedFiles() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> changed;

    for (auto& [key, entry] : m_entries) {
        if (!entry.texture || !entry.texture->valid) continue;

        int64_t diskTime = GetFileWriteTime(key);
        if (diskTime != 0 && diskTime != entry.texture->writeTime) {
            changed.push_back(key);
        }
    }

    return changed;
}

} // namespace rex::graphics
