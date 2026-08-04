#include "NoiseTexture.hpp"
#include <random>
#include <algorithm>

namespace rex::graphics {

bool NoiseTexture::Generate(ID2D1RenderTarget* rt, float opacity) {
    if (!rt) return false;

    const UINT width = 256;
    const UINT height = 256;
    std::vector<BYTE> pixelData(width * height * 4);

    std::mt19937 rng(1337); // Fixed seed for reproducible noise grain
    std::uniform_int_distribution<int> dist(0, 255);

    BYTE baseAlpha = static_cast<BYTE>(opacity * 255.0f);

    for (UINT i = 0; i < width * height; ++i) {
        BYTE val = static_cast<BYTE>(dist(rng));
        pixelData[i * 4 + 0] = val;       // B
        pixelData[i * 4 + 1] = val;       // G
        pixelData[i * 4 + 2] = val;       // R
        pixelData[i * 4 + 3] = baseAlpha; // A
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    HRESULT hr = rt->CreateBitmap(
        D2D1::SizeU(width, height),
        pixelData.data(),
        width * 4,
        &props,
        &m_noiseBitmap
    );

    return SUCCEEDED(hr) && m_noiseBitmap != nullptr;
}

} // namespace rex::graphics
