#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include <vector>

namespace rex::graphics {

/// NoiseTexture creates a tileable 256x256 tactile noise bitmap
/// to simulate realistic frosted glass grain (spec §2.1).
class NoiseTexture {
public:
    NoiseTexture() = default;
    ~NoiseTexture() = default;

    /// Generates noise bitmap onto given Direct2D RenderTarget.
    bool Generate(ID2D1RenderTarget* rt, float opacity = 0.02f);

    /// Gets generated noise bitmap.
    ID2D1Bitmap* GetBitmap() const { return m_noiseBitmap.Get(); }

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_noiseBitmap;
};

} // namespace rex::graphics
