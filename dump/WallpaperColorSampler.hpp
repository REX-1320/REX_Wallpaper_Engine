#pragma once
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>

namespace rex::shell {

/// WallpaperColorSampler decodes a wallpaper image thumbnail, samples an 8x8 grid
/// of pixel RGB values, and computes a dominant ambient tint colour to drive
/// dynamic Liquid Glass light reactivity.
class WallpaperColorSampler {
public:
    WallpaperColorSampler() = default;
    ~WallpaperColorSampler() = default;

    /// Samples dominant colour from image file path.
    D2D1_COLOR_F SampleFromFile(IWICImagingFactory* wicFactory, const std::wstring& imagePath);

    /// Returns cached dominant colour.
    D2D1_COLOR_F GetDominantColor() const { return m_dominantColor; }

private:
    D2D1_COLOR_F m_dominantColor = D2D1::ColorF(0.1f, 0.2f, 0.4f, 0.15f);
};

} // namespace rex::shell
