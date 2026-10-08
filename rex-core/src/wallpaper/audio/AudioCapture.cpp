#include "wallpaper/audio/AudioCapture.hpp"
#include <shared/Logger.hpp>
#include <avrt.h>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "avrt.lib")

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "AudioCapture";
}

AudioCapture::~AudioCapture() {
    Stop();
}

bool AudioCapture::Start() {
    if (m_running.load()) return true;

    HRESULT hr;

    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        REX_LOG_ERROR(kModule, "CoInitializeEx failed (0x{:08x})", hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "MMDeviceEnumerator creation failed (0x{:08x})", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDevice> audioDevice;
    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice);
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "GetDefaultAudioEndpoint failed (0x{:08x})", hr);
        return false;
    }

    hr = audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(m_audioClient.GetAddressOf()));
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "AudioClient activation failed (0x{:08x})", hr);
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "GetMixFormat failed (0x{:08x})", hr);
        return false;
    }

    REFERENCE_TIME hnsPeriod;
    hr = m_audioClient->GetDevicePeriod(nullptr, &hnsPeriod);
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "GetDevicePeriod failed (0x{:08x})", hr);
        return false;
    }

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, hnsPeriod, 0, m_waveFormat, nullptr);
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "AudioClient::Initialize failed (0x{:08x})", hr);
        return false;
    }

    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "GetService(IAudioCaptureClient) failed (0x{:08x})", hr);
        return false;
    }

    m_waveform.clear();
    m_waveform.reserve(kFFTSize * 2);

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "AudioClient::Start failed (0x{:08x})", hr);
        return false;
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&AudioCapture::CaptureThread, this);

    REX_LOG_INFO(kModule, "loopback audio capture started ({}ch, {}Hz)",
                 m_waveFormat->nChannels, m_waveFormat->nSamplesPerSec);
    return true;
}

void AudioCapture::Stop() {
    if (!m_running.load()) return;
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    if (m_audioClient) m_audioClient->Stop();
    m_captureClient.Reset();
    m_audioClient.Reset();
    m_device.Reset();
    m_enumerator.Reset();
    if (m_waveFormat) { CoTaskMemFree(m_waveFormat); m_waveFormat = nullptr; }
    CoUninitialize();
    REX_LOG_INFO(kModule, "loopback audio capture stopped");
}

bool AudioCapture::GetSpectrum(std::array<float, 32>& out) {
    std::lock_guard lock(m_spectrumMutex);
    if (!m_newFrame) return false;
    out = m_spectrum;
    m_newFrame = false;
    return true;
}

void AudioCapture::CaptureThread() {
    // P2-16 drive-by: the NULL task index was an AVRT contract violation
    // (it writes the index through the pointer) — both observed audio-scene
    // crashes died inside AVRT.dll. Revert on thread exit.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristics(L"Audio", &taskIndex);

    while (m_running.load(std::memory_order_acquire)) {
        UINT32 packet = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packet))) {
            Sleep(10);
            continue;
        }
        if (packet == 0) { Sleep(1); continue; }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        HRESULT hr = m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) continue;

        ProcessAudio(data, frames);
        m_captureClient->ReleaseBuffer(frames);
    }

    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
}

void AudioCapture::ProcessAudio(const BYTE* data, UINT32 frames) {
    std::vector<float> mono(frames, 0.0f);

    if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        const float* s = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < frames; ++i) {
            float sum = 0;
            for (WORD ch = 0; ch < m_waveFormat->nChannels; ++ch)
                sum += s[i * m_waveFormat->nChannels + ch];
            mono[i] = sum / m_waveFormat->nChannels;
        }
    } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_PCM ||
               m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        for (UINT32 i = 0; i < frames; ++i) {
            int32_t sum = 0;
            for (WORD ch = 0; ch < m_waveFormat->nChannels; ++ch)
                sum += s[i * m_waveFormat->nChannels + ch];
            mono[i] = static_cast<float>(sum) / (32768.0f * m_waveFormat->nChannels);
        }
    }

    {
        std::lock_guard lock(m_audioMutex);
        m_waveform.insert(m_waveform.end(), mono.begin(), mono.end());
        if (m_waveform.size() > static_cast<size_t>(kFFTSize) * 2) {
            m_waveform.erase(m_waveform.begin(),
                m_waveform.begin() + (m_waveform.size() - kFFTSize));
        }
    }

    if (m_waveform.size() >= static_cast<size_t>(kFFTSize)) {
        ComputeFFT();
    }
}

void AudioCapture::ComputeFFT() {
    // In-place real FFT (Cooley-Tukey, radix-2)
    std::vector<float> re(kFFTSize);
    std::vector<float> im(kFFTSize, 0.0f);

    {
        std::lock_guard lock(m_audioMutex);
        auto wavEnd = m_waveform.end();
        for (int i = 0; i < kFFTSize; ++i) {
            float hann = 0.5f * (1.0f - cosf(6.2831853f * i / (kFFTSize - 1)));
            re[i] = *(wavEnd - kFFTSize + i) * hann;
        }
    }

    // Bit-reversal permutation
    for (int i = 1, j = 0; i < kFFTSize; ++i) {
        int bit = kFFTSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // Butterfly stages
    for (int len = 2; len <= kFFTSize; len <<= 1) {
        float ang = -6.283185307f / len;
        float wlen_r = cosf(ang), wlen_i = sinf(ang);
        for (int i = 0; i < kFFTSize; i += len) {
            float w_r = 1.0f, w_i = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                float u_r = re[i + j], u_i = im[i + j];
                float v_r = re[i + j + len/2] * w_r - im[i + j + len/2] * w_i;
                float v_i = re[i + j + len/2] * w_i + im[i + j + len/2] * w_r;
                re[i + j] = u_r + v_r; im[i + j] = u_i + v_i;
                re[i + j + len/2] = u_r - v_r; im[i + j + len/2] = u_i - v_i;
                float t = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = t;
            }
        }
    }

    // Bin into 32 bands
    std::array<float, 32> spectrum{};
    int samplesPerBand = (kFFTSize / 2) / 32;
    for (int b = 0; b < 32; ++b) {
        float sum = 0;
        for (int j = 0; j < samplesPerBand; ++j) {
            int idx = b * samplesPerBand + j;
            if (idx < kFFTSize / 2) {
                float mag = sqrtf(re[idx] * re[idx] + im[idx] * im[idx]);
                sum += mag;
            }
        }
        spectrum[b] = std::min((sum / samplesPerBand) * 2.0f, 1.0f);
    }

    std::lock_guard lock(m_spectrumMutex);
    m_spectrum = spectrum;
    m_newFrame = true;
}

} // namespace rex::wallpaper
