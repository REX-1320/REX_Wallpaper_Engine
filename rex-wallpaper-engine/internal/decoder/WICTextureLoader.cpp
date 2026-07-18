#include "WICTextureLoader.hpp"

#include <shared/Logger.hpp>

#include <wincodec.h>
#include <wrl/client.h>

#include <codecvt>
#include <locale>

namespace rex::wallpaper {

namespace {

    // RAII COM guard — initializes COM on construction, uninitializes on destruction.
    // Skips uninit if COM was already initialized on this thread with a different mode.
    class ComGuard {
    public:
        ComGuard() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            m_initialized = SUCCEEDED(hr);
            if (!m_initialized && hr == RPC_E_CHANGED_MODE) {
                // COM already initialized with a different threading model; just use it
                m_initialized = false;
            }
        }
        ~ComGuard() {
            if (m_initialized) {
                CoUninitialize();
            }
        }
        ComGuard(const ComGuard&) = delete;
        ComGuard& operator=(const ComGuard&) = delete;

    private:
        bool m_initialized = false;
    };

    // Converts UTF-8 string_view to wide string for WIC APIs.
    std::wstring ToWide(std::string_view sv) {
        if (sv.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), result.data(), len);
        return result;
    }

} // anonymous namespace

TextureData DecodeTextureFromFile(std::string_view path) {
    TextureData result;
    ComGuard com;

    // Create WIC imaging factory
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CoCreateInstance(IWICImagingFactory) failed: {:08x}", hr);
        return result;
    }

    // Create decoder
    std::wstring wpath = ToWide(path);
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        wpath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CreateDecoderFromFilename failed: {:08x} (path: {})",
                      hr, path);
        return result;
    }

    // Get first frame
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "GetFrame(0) failed: {:08x}", hr);
        return result;
    }

    // Convert to RGBA8
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CreateFormatConverter failed: {:08x}", hr);
        return result;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "FormatConverter::Initialize failed: {:08x}", hr);
        return result;
    }

    // Get dimensions
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.rowPitch = result.width * 4; // RGBA8 = 4 bytes per pixel

    // Copy pixels
    UINT bufferSize = result.rowPitch * result.height;
    result.pixels.resize(bufferSize);

    hr = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(result.rowPitch),
        bufferSize,
        result.pixels.data());

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CopyPixels failed: {:08x}", hr);
        result.pixels.clear();
        result.width = 0;
        result.height = 0;
        result.rowPitch = 0;
        return result;
    }

    REX_LOG_INFO("WICLoader", "Decoded {}x{} image ({} bytes) from {}",
                 result.width, result.height, bufferSize, path);

    return result;
}

} // namespace rex::wallpaper
