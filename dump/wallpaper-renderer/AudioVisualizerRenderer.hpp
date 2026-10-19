#pragma once
#include "IRenderer.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <thread>
#include <string>

class Compositor;

namespace rex::wallpaper {

class AudioVisualizerRenderer : public IRenderer {
public:
    AudioVisualizerRenderer();
    ~AudioVisualizerRenderer() override;

    bool load(const SceneInfo& scene, Compositor& compositor) override;
    void update(double deltaTime) override;
    void render(Compositor& compositor) override;
    void unload() override;

    void play() override { m_playing.store(true); }
    void pause() override { m_playing.store(false); }
    void stop() override { m_playing.store(false); }
    bool isPlaying() const override { return m_playing.load(); }

private:
    bool InitD3D12(Compositor& compositor, int width, int height);
    bool InitAudioCapture();
    void AudioCaptureThread();
    void ProcessAudioBuffer(const std::vector<float>& samples);
    void DrawBars();
    void DrawWaveform();
    void DrawParticles();
    void CompileShaders();

    enum class VisualType { Bars, Waveform, Particles };
    VisualType m_visualType = VisualType::Bars;

    // D3D12
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    HANDLE m_sharedHandle = nullptr;
    HANDLE m_fenceHandle = nullptr;
    UINT64 m_fenceValue = 1;

    // Pipeline for full-screen triangle
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBars;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoWaveform;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoParticles;

    // Spectrum upload buffer (mapped)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    float* m_mappedSpectrum = nullptr;
    UINT m_numBars = 32;

    // Audio
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enum;
    Microsoft::WRL::ComPtr<IMMDevice> m_audioDevice;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_waveFormat = nullptr;
    std::atomic<bool> m_audioRunning{false};
    std::thread m_audioThread;

    // Buffers
    std::vector<float> m_audioBuffer;
    std::vector<float> m_spectrum;
    std::vector<float> m_waveform;
    std::atomic<bool> m_newFrame{false};
    std::mutex m_audioMutex;

    int m_width = 0, m_height = 0;
    std::atomic<bool> m_playing{false};
    float m_time = 0.0f;
    Compositor* m_compositor = nullptr;
    static constexpr uint64_t AV_LAYER_ID = 4;
    static constexpr int FFT_SIZE = 1024;
};

} // namespace
