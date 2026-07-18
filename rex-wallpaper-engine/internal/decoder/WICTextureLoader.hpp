#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

struct TextureData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int rowPitch = 0; // bytes per row (RGBA8 = width * 4)
};

/// Decodes an image file (JPG, PNG, BMP, TIFF) via Windows Imaging Component.
/// Returns RGBA8 pixel data, or an empty TextureData on failure.
/// The calling thread must have COM initialized (COINIT_APARTMENTTHREADED or
/// COINIT_MULTITHREADED).
TextureData DecodeTextureFromFile(std::string_view path);

} // namespace rex::wallpaper
