#include "WallpaperColorSampler.hpp"
#include <vector>
#include <algorithm>

namespace rex::shell {

D2D1_COLOR_F WallpaperColorSampler::SampleFromFile(IWICImagingFactory* wicFactory, const std::wstring& imagePath) {
    if (!wicFactory || imagePath.empty()) {
        return m_dominantColor;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromFilename(
        imagePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) return m_dominantColor;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) return m_dominantColor;

    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    hr = wicFactory->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || !scaler) return m_dominantColor;

    // Scale down to 8x8 grid for fast sampling
    hr = scaler->Initialize(frame.Get(), 8, 8, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return m_dominantColor;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) return m_dominantColor;

    hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return m_dominantColor;

    BYTE pixels[8 * 8 * 4] = {};
    hr = converter->CopyPixels(nullptr, 8 * 4, sizeof(pixels), pixels);
    if (FAILED(hr)) return m_dominantColor;

    // Calculate average RGB values across the 64 sampled cells
    double sumR = 0, sumG = 0, sumB = 0;
    for (int i = 0; i < 64; ++i) {
        sumB += pixels[i * 4 + 0];
        sumG += pixels[i * 4 + 1];
        sumR += pixels[i * 4 + 2];
    }

    float avgR = static_cast<float>((sumR / 64.0) / 255.0);
    float avgG = static_cast<float>((sumG / 64.0) / 255.0);
    float avgB = static_cast<float>((sumB / 64.0) / 255.0);

    // Apply low opacity for glass tinting
    m_dominantColor = D2D1::ColorF(avgR, avgG, avgB, 0.20f);
    return m_dominantColor;
}

} // namespace rex::shell
