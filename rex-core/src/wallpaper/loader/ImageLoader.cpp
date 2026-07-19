#include "ImageLoader.hpp"

#include <shared/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace rex::wallpaper::loader {

namespace {

    std::string Extension(std::string_view path) {
        auto dot = path.rfind('.');
        if (dot == std::string_view::npos) return "";
        std::string ext(path.substr(dot));
        // lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return ext;
    }

} // anonymous namespace

// ─── Construction / Destruction ─────────────────────────────────────────────

ImageLoader::ImageLoader() = default;

ImageLoader::~ImageLoader() {
    StopHotReload();
}

// ─── Loading ────────────────────────────────────────────────────────────────

shared::Result<const graphics::CachedTexture*> ImageLoader::Load(
    std::string_view path)
{
    if (path.empty()) {
        return shared::Result<const graphics::CachedTexture*>(
            shared::ErrorCode::InvalidArgument,
            "Empty path");
    }

    if (!IsFormatSupported(path)) {
        return shared::Result<const graphics::CachedTexture*>(
            shared::ErrorCode::UnsupportedFormat,
            "Unsupported image format: " + std::string(path));
    }

    return m_cache.GetOrLoad(path);
}

shared::Result<const graphics::CachedTexture*> ImageLoader::Reload(
    std::string_view path)
{
    return m_cache.Reload(path);
}

void ImageLoader::Unload(std::string_view path) {
    m_cache.Evict(path);
}

void ImageLoader::Clear() {
    m_cache.Clear();
}

// ─── Queries ────────────────────────────────────────────────────────────────

bool ImageLoader::IsLoaded(std::string_view path) const {
    return m_cache.Contains(path);
}

const graphics::CachedTexture* ImageLoader::Get(
    std::string_view path) const
{
    return m_cache.Get(path);
}

ImageFormat ImageLoader::DetectFormat(std::string_view path) {
    std::string ext = Extension(path);

    if (ext == ".png")  return ImageFormat::PNG;
    if (ext == ".jpg" || ext == ".jpeg") return ImageFormat::JPEG;
    if (ext == ".bmp")  return ImageFormat::BMP;
    if (ext == ".tiff" || ext == ".tif") return ImageFormat::TIFF;
    if (ext == ".webp") return ImageFormat::WEBP;
    if (ext == ".gif")  return ImageFormat::GIF;
    if (ext == ".ico")  return ImageFormat::ICO;

    return ImageFormat::Unknown;
}

bool ImageLoader::IsFormatSupported(std::string_view path) {
    return DetectFormat(path) != ImageFormat::Unknown;
}

size_t ImageLoader::CacheSize() const {
    return m_cache.Size();
}

// ─── Hot-reload ─────────────────────────────────────────────────────────────

void ImageLoader::StartHotReload(
    std::chrono::milliseconds intervalMs,
    ChangeCallback onChange)
{
    if (m_hotReloadRunning.load()) return;

    m_changeCallback = std::move(onChange);
    m_pollInterval   = intervalMs;
    m_hotReloadRunning.store(true, std::memory_order_release);

    m_hotReloadThread = std::thread([this]() {
        REX_LOG_INFO("ImageLoader", "Hot-reload started ({}ms)",
                     m_pollInterval.count());

        while (m_hotReloadRunning.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(m_pollInterval);

            auto changed = m_cache.PollChangedFiles();
            for (const auto& path : changed) {
                auto result = m_cache.Reload(path);
                if (result.IsSuccess() && m_changeCallback) {
                    REX_LOG_INFO("ImageLoader",
                        "Hot-reload detected change: {}", path);
                    m_changeCallback(path);
                }
            }
        }

        REX_LOG_INFO("ImageLoader", "Hot-reload stopped");
    });
}

void ImageLoader::StopHotReload() {
    if (!m_hotReloadRunning.load()) return;

    m_hotReloadRunning.store(false, std::memory_order_release);
    if (m_hotReloadThread.joinable()) {
        m_hotReloadThread.join();
    }
}

std::vector<std::string> ImageLoader::PollChanges() {
    auto changed = m_cache.PollChangedFiles();
    for (const auto& path : changed) {
        m_cache.Reload(path);
    }
    return changed;
}

} // namespace rex::wallpaper::loader
