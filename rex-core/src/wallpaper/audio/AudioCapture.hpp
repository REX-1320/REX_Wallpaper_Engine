#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace rex::wallpaper {

// ─── Capture format classification ───────────────────────────────────────
//
// The shared-mode mix format is frequently WAVE_FORMAT_EXTENSIBLE (any
// >2-channel endpoint, and most float32 endpoints), whose REAL sample type
// lives in the SubFormat GUID — not in wFormatTag. Treating EXTENSIBLE as
// PCM int16 reinterpreted float32 bytes as int16 (garbage spectrum bands;
// audit 2026-10-17 AU2).
//
// The GUIDs are local constants — referencing the SDK's extern
// KSDATAFORMAT_SUBTYPE_* symbols would drag in uuid.lib; same practice as
// the decoder's DXVA profile table. Values are the standard serialized
// {0000000X-0000-0010-8000-00AA00389B71} forms.
inline constexpr GUID kCaptureSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
inline constexpr GUID kCaptureSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

inline bool CaptureGuidEquals(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
}

enum class CaptureSampleFormat { Unsupported, Float32, S16, S24, S32 };

inline CaptureSampleFormat ClassifyCaptureFormat(const WAVEFORMATEX* wf) {
    if (!wf) return CaptureSampleFormat::Unsupported;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return CaptureSampleFormat::Float32;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wf->cbSize <
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return CaptureSampleFormat::Unsupported;  // SubFormat absent
        }
        const GUID& sub =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->SubFormat;
        if (CaptureGuidEquals(sub, kCaptureSubtypeIeeeFloat)) {
            return CaptureSampleFormat::Float32;
        }
        if (!CaptureGuidEquals(sub, kCaptureSubtypePcm)) {
            // e.g. passthrough/compressed subtypes: reinterpretation would
            // feed the FFT garbage — silence beats garbage.
            return CaptureSampleFormat::Unsupported;
        }
        // EXTENSIBLE + PCM: fall through to the container-bits switch.
    } else if (wf->wFormatTag != WAVE_FORMAT_PCM) {
        return CaptureSampleFormat::Unsupported;
    }
    switch (wf->wBitsPerSample) {
        case 16: return CaptureSampleFormat::S16;
        case 24: return CaptureSampleFormat::S24;
        case 32: return CaptureSampleFormat::S32;
        default: return CaptureSampleFormat::Unsupported;
    }
}

/// True when a capture HRESULT means the bound device is gone and the client
/// must be torn down and re-opened (default-device switch/unplug/service
/// restart). Any other failure is transient and must not churn the client.
inline bool CaptureNeedsReopen(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

/// True when a captured packet carries no usable samples: a null buffer or
/// the AUDCLNT_BUFFERFLAGS_SILENT flag. The consumer contributes zeros for
/// the packet's frame count instead of reading the buffer (audit AU8).
inline bool CaptureFrameIsSilent(const BYTE* data, DWORD flags) {
    return data == nullptr || (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
}

// Captures loopback audio via WASAPI and computes an FFT spectrum.
// Thread-safe; the render thread reads the latest spectrum.
class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /// Start capturing loopback audio. Returns false on failure.
    bool Start();
    /// Stop capturing and release resources.
    void Stop();

    /// Returns true if new spectrum data is available since last call.
    /// Copies the latest 32-band spectrum into `out`.
    bool GetSpectrum(std::array<float, 32>& out);

    bool IsRunning() const { return m_running.load(); }

private:
    void CaptureThread();
    void ProcessAudio(const BYTE* data, UINT32 frames);
    void ComputeFFT();

    /// Creates enumerator/device/client/capture-client and the mix format on
    /// the CALLING thread (COM must already be initialized there). Resets any
    /// previous generation first, so it doubles as the re-open path.
    bool OpenClient();
    /// Capture-thread device-change recovery: stop + OpenClient + Start.
    /// False leaves the client torn down for the next retry.
    bool ReopenDevice();

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // WASAPI
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_waveFormat = nullptr;
    // True when Start()'s CoInitializeEx actually added an apartment ref on
    // the calling thread (S_OK/S_FALSE). RPC_E_CHANGED_MODE means the thread
    // already had COM in another mode and no ref was added — calling
    // CoUninitialize then would underflow the counter (audit AU5).
    bool m_comInitialized = false;

    // Audio buffer
    static constexpr int kFFTSize = 2048;
    std::vector<float> m_waveform;
    std::mutex m_audioMutex;

    // Spectrum output
    std::mutex m_spectrumMutex;
    std::array<float, 32> m_spectrum{};
    bool m_newFrame = false;
};

} // namespace rex::wallpaper
