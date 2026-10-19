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

bool AudioCapture::OpenClient() {
    // Requires COM on the calling thread. Resets any previous generation so
    // this doubles as the device-change re-open path.
    m_captureClient.Reset();
    m_audioClient.Reset();
    m_enumerator.Reset();
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
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
    return true;
}

bool AudioCapture::Start() {
    if (m_running.load()) return true;

    // Everything acquired before m_running flips true must be released on the
    // failure paths: Stop() early-returns while not running, so a failed
    // Start used to leak the WAVEFORMATEX and leave the COM apartment
    // initialized on this thread forever (audit 2026-10-17 AU2).
    bool coInitialized = false;
    auto fail = [this, &coInitialized]() {
        m_captureClient.Reset();
        m_audioClient.Reset();
        m_enumerator.Reset();
        if (m_waveFormat) {
            CoTaskMemFree(m_waveFormat);
            m_waveFormat = nullptr;
        }
        if (coInitialized) {
            CoUninitialize();
            coInitialized = false;
        }
        return false;
    };

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        REX_LOG_ERROR(kModule, "CoInitializeEx failed (0x{:08x})", hr);
        return false;
    }
    coInitialized = SUCCEEDED(hr);

    if (!OpenClient()) return fail();

    m_waveform.clear();
    m_waveform.reserve(kFFTSize * 2);

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "AudioClient::Start failed (0x{:08x})", hr);
        return fail();
    }

    // Only NOW does the caller thread own an apartment ref that Stop() must
    // release. RPC_E_CHANGED_MODE above means no ref was added.
    m_comInitialized = coInitialized;
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
    m_enumerator.Reset();
    if (m_waveFormat) { CoTaskMemFree(m_waveFormat); m_waveFormat = nullptr; }
    // Paired with Start()'s CoInitializeEx: only release when that call
    // actually added a ref (RPC_E_CHANGED_MODE added none — audit AU5).
    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
    REX_LOG_INFO(kModule, "loopback audio capture stopped");
}

bool AudioCapture::GetSpectrum(std::array<float, 32>& out) {
    std::lock_guard lock(m_spectrumMutex);
    if (!m_newFrame) return false;
    out = m_spectrum;
    m_newFrame = false;
    return true;
}

bool AudioCapture::ReopenDevice() {
    // Capture-thread only. Stop() joins this thread before touching any of
    // these members, so the unsynchronized mutation cannot race the caller.
    if (m_audioClient) m_audioClient->Stop();  // best-effort: device is gone
    if (!OpenClient()) return false;
    if (FAILED(m_audioClient->Start())) {
        REX_LOG_WARN(kModule, "re-opened capture client failed to start");
        return false;
    }
    REX_LOG_INFO(kModule, "loopback capture re-opened ({}ch, {}Hz)",
                 m_waveFormat->nChannels, m_waveFormat->nSamplesPerSec);
    return true;
}

void AudioCapture::CaptureThread() {
    // WASAPI/COM: the clients below are created on the caller thread, but ALL
    // calls happen here, so this thread owns its own apartment. MTA because
    // nothing on this thread pumps messages (audit AU6). SUCCEEDED covers
    // S_FALSE ("already initialized") — still an owned ref to release.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOk = SUCCEEDED(coHr);

    // P2-16 drive-by: the NULL task index was an AVRT contract violation
    // (it writes the index through the pointer) — both observed audio-scene
    // crashes died inside AVRT.dll. Revert on thread exit.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristics(L"Audio", &taskIndex);

    while (m_running.load(std::memory_order_acquire)) {
        UINT32 packet = 0;
        HRESULT pktHr = m_captureClient
                            ? m_captureClient->GetNextPacketSize(&packet)
                            : E_FAIL;
        if (FAILED(pktHr)) {
            // A default-device switch/unplug invalidates the bound client;
            // the old code Sleep(10)'d forever and the spectrum froze
            // silently (audit AU7). Other failures are transient.
            if (CaptureNeedsReopen(pktHr)) {
                REX_LOG_WARN(kModule, "capture device lost (0x{:08x}); re-opening",
                             static_cast<unsigned>(pktHr));
                if (!ReopenDevice()) Sleep(1000);  // retry on the next pass
            } else {
                Sleep(10);
            }
            continue;
        }
        if (packet == 0) { Sleep(1); continue; }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        HRESULT hr = m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            if (CaptureNeedsReopen(hr)) {
                REX_LOG_WARN(kModule, "capture buffer device loss (0x{:08x}); re-opening",
                             static_cast<unsigned>(hr));
                if (!ReopenDevice()) Sleep(1000);
            }
            continue;
        }

        // SILENT packets and null buffers are silence — feed zeros so the
        // waveform keeps advancing and the spectrum decays instead of
        // freezing at the last loud frame (audit AU8).
        ProcessAudio(CaptureFrameIsSilent(data, flags) ? nullptr : data, frames);
        m_captureClient->ReleaseBuffer(frames);
    }

    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    if (coOk) {
        CoUninitialize();
    }
}

void AudioCapture::ProcessAudio(const BYTE* data, UINT32 frames) {
    std::vector<float> mono(frames, 0.0f);

    // The sample type comes from ClassifyCaptureFormat (SubFormat-aware for
    // WAVE_FORMAT_EXTENSIBLE — see the header): reading float32 payloads as
    // int16 produced garbage spectrum bands on every endpoint whose shared
    // mix format is float (audit AU2).
    const int nCh = m_waveFormat ? m_waveFormat->nChannels : 0;
    if (data && nCh > 0) {
        switch (ClassifyCaptureFormat(m_waveFormat)) {
            case CaptureSampleFormat::Float32: {
                const float* s = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    float sum = 0.0f;
                    for (int ch = 0; ch < nCh; ++ch) sum += s[i * nCh + ch];
                    mono[i] = sum / nCh;
                }
                break;
            }
            case CaptureSampleFormat::S16: {
                const int16_t* s = reinterpret_cast<const int16_t*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    int32_t sum = 0;
                    for (int ch = 0; ch < nCh; ++ch) sum += s[i * nCh + ch];
                    mono[i] = static_cast<float>(sum) / (32768.0f * nCh);
                }
                break;
            }
            case CaptureSampleFormat::S24: {
                // Tightly packed 3-byte little-endian signed samples.
                for (UINT32 i = 0; i < frames; ++i) {
                    const BYTE* p = data + static_cast<size_t>(i) * nCh * 3;
                    int32_t sum = 0;
                    for (int ch = 0; ch < nCh; ++ch) {
                        const BYTE* s = p + static_cast<size_t>(ch) * 3;
                        int32_t v = s[0] | (s[1] << 8) | (s[2] << 16);
                        if (v & 0x800000) v -= 0x1000000;
                        sum += v;
                    }
                    mono[i] = static_cast<float>(sum) / (8388608.0f * nCh);
                }
                break;
            }
            case CaptureSampleFormat::S32: {
                // Normalise per sample as float: summing raw int32 would
                // overflow for wide layouts.
                const int32_t* s = reinterpret_cast<const int32_t*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    float sum = 0.0f;
                    for (int ch = 0; ch < nCh; ++ch) {
                        sum += static_cast<float>(s[i * nCh + ch]) /
                               2147483648.0f;
                    }
                    mono[i] = sum / nCh;
                }
                break;
            }
            case CaptureSampleFormat::Unsupported:
                break;  // stays silent rather than feeding garbage
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
