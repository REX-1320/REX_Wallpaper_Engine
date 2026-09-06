#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace rex::wallpaper {

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

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // WASAPI
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> m_device;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_waveFormat = nullptr;

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
