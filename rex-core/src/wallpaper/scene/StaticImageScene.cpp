#include "StaticImageScene.hpp"

#include <shared/Logger.hpp>

namespace rex::wallpaper::scene {

// ─── Construction / Destruction ─────────────────────────────────────────────

StaticImageScene::StaticImageScene(
    std::shared_ptr<loader::ImageLoader> imageLoader)
    : m_imageLoader(std::move(imageLoader))
{
}

StaticImageScene::~StaticImageScene() {
    Unload();
}

// ─── Load ───────────────────────────────────────────────────────────────────

shared::Result<void> StaticImageScene::Load(std::string_view path) {
    if (path.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
            "Empty image path");
    }

    REX_LOG_INFO("StaticImageScene", "Loading: {}", path);

    auto result = m_imageLoader->Load(path);
    if (result.IsError()) {
        return shared::Result<void>(
            result.Error().code, result.Error().message);
    }

    m_texture = result.Value();
    m_width   = m_texture->pixels.width;
    m_height  = m_texture->pixels.height;
    m_version = m_texture->version;
    m_path    = path;
    m_loaded  = true;

    REX_LOG_INFO("StaticImageScene",
        "Loaded {}x{} image (v{})", m_width, m_height, m_version);

    return shared::Result<void>();
}

// ─── Reload ─────────────────────────────────────────────────────────────────

shared::Result<void> StaticImageScene::Reload() {
    if (m_path.empty()) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
            "No image loaded");
    }

    auto result = m_imageLoader->Reload(m_path);
    if (result.IsError()) {
        return shared::Result<void>(
            result.Error().code, result.Error().message);
    }

    m_texture = result.Value();
    m_width   = m_texture->pixels.width;
    m_height  = m_texture->pixels.height;
    m_version = m_texture->version;

    REX_LOG_INFO("StaticImageScene",
        "Reloaded {}x{} (v{})", m_width, m_height, m_version);

    return shared::Result<void>();
}

// ─── Unload ─────────────────────────────────────────────────────────────────

void StaticImageScene::Unload() {
    m_loaded  = false;
    m_texture = nullptr;
    m_width   = 0;
    m_height  = 0;
    m_path.clear();
}

// ─── Pixel data access ─────────────────────────────────────────────────────

const uint8_t* StaticImageScene::GetPixels() const {
    if (!m_texture || !m_loaded) return nullptr;
    return m_texture->pixels.pixels.data();
}

int StaticImageScene::GetStride() const {
    if (!m_texture || !m_loaded) return 0;
    // RGBA8 = 4 bytes per pixel
    return 4;
}

int StaticImageScene::GetRowPitch() const {
    if (!m_texture || !m_loaded) return 0;
    return m_texture->pixels.rowPitch;
}

} // namespace rex::wallpaper::scene
