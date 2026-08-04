#include "AudioVisualizerRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>
#include <cmath>
#include <d3dcompiler.h>
#include <avrt.h>
#include <algorithm>

namespace {

// Real Cooley-Tukey FFT in-place
void Fft(std::vector<float>& re, std::vector<float>& im) {
    int n = (int)re.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.283185307f / len;
        float wlen_r = cosf(ang), wlen_i = sinf(ang);
        for (int i = 0; i < n; i += len) {
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
}

// Inline HLSL shaders
const char* g_vsSrc = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut mainVS(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

const char* g_psSrc = R"(
cbuffer C : register(b0) {
    float4 uTimeRes;   // (time, resX, resY, mode)
    float4 uNumBars;   // (numBars, 0,0,0)
    float4 uBars[8];   // 32 spectrum values packed as float4
};
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 mainPS(PSIn i) : SV_Target {
    float t = uTimeRes.x;
    float2 res = uTimeRes.yz;
    float mode = uTimeRes.w;
    int nb = (int)uNumBars.x;
    float ar = res.x / res.y;
    float2 uv = i.uv;
    uv.x *= ar;

    float bars[32];
    [unroll] for (int k = 0; k < 32; ++k)
        bars[k] = uBars[k/4][k%4];

    // Bars
    if (mode < 0.5f) {
        float bw = ar / nb;
        int idx = (int)(uv.x / bw);
        float lx = frac(uv.x / bw);
        if (idx >= 0 && idx < nb) {
            float h = bars[idx] * 0.8f;
            if (uv.y < h && lx > 0.1f && lx < 0.9f) {
                float3 col = float3(0.1f, 0.3f, 0.5f) + bars[idx] * float3(0.3f, 0.3f, 0.5f);
                return float4(col, 1);
            }
        }
        return float4(0,0,0,1);
    }

    // Waveform
    if (mode < 1.5f) {
        float xp = uv.x / ar;
        int idx = (int)(xp * nb);
        idx = clamp(idx, 0, nb-1);
        float waveY = bars[idx] * 0.4f + 0.5f;
        float d = abs(uv.y - waveY);
        if (d < 0.008f)
            return float4(0.2f, 0.8f, 0.4f, 1);
        return float4(0.02f, 0.02f, 0.03f, 1);
    }

    // Particles
    {
        float3 col = float3(0,0,0);
        for (int i = 0; i < nb; ++i) {
            float a = bars[i] * 6.283f + t;
            float r = 0.08f + bars[i] * 0.12f;
            float2 cp = float2(0.5f*ar, 0.5f);
            float2 pp = cp + float2(cos(a), sin(a)) * r;
            float2 diff = uv - pp;
            float dd = length(diff);
            if (dd < 0.02f) {
                float intensity = bars[i] * 2.0f;
                col += float3(0.3f, 0.1f, 0.6f) * intensity * (1 - dd/0.02f);
            }
        }
        return float4(col, 1);
    }
}
)";

} // anonymous namespace

namespace rex::wallpaper {

AudioVisualizerRenderer::AudioVisualizerRenderer() = default;
AudioVisualizerRenderer::~AudioVisualizerRenderer() { unload(); }

bool AudioVisualizerRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_width  = static_cast<int>(compositor.GetViewportWidth());
    m_height = static_cast<int>(compositor.GetViewportHeight());
    m_compositor = &compositor;

    if (scene.type == "audio.waveform") m_visualType = VisualType::Waveform;
    else if (scene.type == "audio.particles") m_visualType = VisualType::Particles;
    else m_visualType = VisualType::Bars;

    if (!InitD3D12(compositor, m_width, m_height)) return false;
    if (!InitAudioCapture()) return false;

    compositor.SubmitLayer({ AV_LAYER_ID, 0, nullptr,
        { 0, 0, (float)m_width, (float)m_height }, 1.0f, 0, true });

    m_playing = true;
    printf("[AudioVisualizer] loaded (mode=%d)\n", (int)m_visualType);
    return true;
}

void AudioVisualizerRenderer::update(double deltaTime) {
    m_time += (float)deltaTime;
}

void AudioVisualizerRenderer::render(Compositor& compositor) {
    if (!m_playing.load() || !m_device) return;

    switch (m_visualType) {
        case VisualType::Bars:       DrawBars();      break;
        case VisualType::Waveform:   DrawWaveform();  break;
        case VisualType::Particles:  DrawParticles(); break;
    }

    m_commandQueue->Signal(m_sharedFence.Get(), m_fenceValue);
    compositor.WaitForExternalFence(m_fenceHandle, m_fenceValue);
}

void AudioVisualizerRenderer::unload() {
    m_playing = false;
    m_audioRunning = false;
    if (m_audioThread.joinable()) m_audioThread.join();

    if (m_audioClient) m_audioClient->Stop();

    if (m_compositor && m_device) {
        m_compositor->RemoveLayerTexture(AV_LAYER_ID);
        m_compositor->RemoveLayer(AV_LAYER_ID);
        m_compositor = nullptr;
    }

    if (m_uploadBuffer && m_mappedSpectrum) {
        m_uploadBuffer->Unmap(0, nullptr);
        m_mappedSpectrum = nullptr;
    }

    m_uploadBuffer.Reset();
    m_psoBars.Reset();
    m_psoWaveform.Reset();
    m_psoParticles.Reset();
    m_rootSig.Reset();
    m_sharedTexture.Reset();
    m_rtvHeap.Reset();
    m_sharedFence.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    if (m_fenceHandle) { CloseHandle(m_fenceHandle); m_fenceHandle = nullptr; }
    if (m_sharedHandle) { CloseHandle(m_sharedHandle); m_sharedHandle = nullptr; }

    if (m_waveFormat) CoTaskMemFree(m_waveFormat);
    m_audioClient.Reset();
    m_captureClient.Reset();
    m_audioDevice.Reset();
    m_enum.Reset();
    CoUninitialize();
}

// ─── D3D12 Initialization ──────────────────────────────────────────────

bool AudioVisualizerRenderer::InitD3D12(Compositor& compositor, int width, int height) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&m_device)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_commandQueue))))
        return false;

    // Shared texture
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)width;
    td.Height = (UINT)height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED,
        &td, D3D12_RESOURCE_STATE_COMMON, &cv,
        IID_PPV_ARGS(&m_sharedTexture)))) return false;

    if (FAILED(m_device->CreateSharedHandle(m_sharedTexture.Get(), nullptr,
        GENERIC_ALL, L"REXAVFrame", &m_sharedHandle))) return false;
    compositor.ImportLayerTexture(AV_LAYER_ID, m_sharedHandle, width, height);

    // Fence
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&m_sharedFence)))) return false;
    if (FAILED(m_device->CreateSharedHandle(m_sharedFence.Get(), nullptr,
        GENERIC_ALL, L"REXAVFence", &m_fenceHandle))) return false;

    // RTV
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc,
        IID_PPV_ARGS(&m_rtvHeap)))) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(m_sharedTexture.Get(), &rtv,
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // Command allocator + list
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList))))
        return false;
    m_commandList->Close();

    // Upload buffer for spectrum (256-byte aligned CB for 32 floats + header)
    UINT cbSize = 256; // 4*8*4 + 4*4 = 144, round to 256
    D3D12_HEAP_PROPERTIES uph = {};
    uph.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ubd = {};
    ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ubd.Width = cbSize;
    ubd.Height = 1;
    ubd.DepthOrArraySize = 1;
    ubd.MipLevels = 1;
    ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&uph, D3D12_HEAP_FLAG_NONE,
        &ubd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer)))) return false;
    m_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedSpectrum));

    // Compile shaders
    CompileShaders();

    printf("[AudioVisualizer] D3D12 initialized\n");
    return true;
}

void AudioVisualizerRenderer::CompileShaders() {
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    auto compile = [&](const char* src, const char* target, ID3DBlob** blob) {
        if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
            "main" + 0, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, blob, &errBlob))) {
            if (errBlob) printf("[AV] shader error: %s\n",
                (const char*)errBlob->GetBufferPointer());
            return false;
        }
        return true;
    };

    if (!compile(g_vsSrc, "vs_5_0", vsBlob.GetAddressOf()) ||
        !compile(g_psSrc, "ps_5_0", psBlob.GetAddressOf()))
        return;

    // Root signature: CBV b0 (spectrum data)
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.RegisterSpace = 0;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
        rsBlob.GetAddressOf(), &errBlob))) {
        if (errBlob) printf("[AV] root sig error: %s\n",
            (const char*)errBlob->GetBufferPointer());
        return;
    }
    m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
        rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psd = {};
    psd.pRootSignature = m_rootSig.Get();
    psd.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psd.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psd.NumRenderTargets = 1;
    psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psd.SampleDesc.Count = 1;

    // All three modes share the same PSO (mode select in CB)
    m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_psoBars));
    m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_psoWaveform));
    m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_psoParticles));
}

// ─── Audio Capture ────────────────────────────────────────────────────

bool AudioVisualizerRenderer::InitAudioCapture() {
    HRESULT hr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&m_enum));
    if (FAILED(hr)) { printf("[AV] no MMDeviceEnumerator\n"); return false; }

    hr = m_enum->GetDefaultAudioEndpoint(eRender, eConsole, &m_audioDevice);
    if (FAILED(hr)) { printf("[AV] no default audio endpoint\n"); return false; }

    hr = m_audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(m_audioClient.GetAddressOf()));
    if (FAILED(hr)) { printf("[AV] no audio client\n"); return false; }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) return false;

    REFERENCE_TIME hnsPeriod;
    hr = m_audioClient->GetDevicePeriod(nullptr, &hnsPeriod);
    if (FAILED(hr)) return false;

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, hnsPeriod, 0, m_waveFormat, nullptr);
    if (FAILED(hr)) { printf("[AV] initialize failed: %08x\n", hr); return false; }

    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) return false;

    hr = m_audioClient->Start();
    if (FAILED(hr)) return false;

    m_audioRunning = true;
    m_audioThread = std::thread(&AudioVisualizerRenderer::AudioCaptureThread, this);
    printf("[AV] audio capture started\n");
    return true;
}

void AudioVisualizerRenderer::AudioCaptureThread() {
    AvSetMmThreadCharacteristics(L"Audio", nullptr);

    while (m_audioRunning.load()) {
        UINT32 packet = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packet))) {
            Sleep(10); continue;
        }
        if (packet == 0) { Sleep(1); continue; }

        BYTE* data;
        UINT32 frames;
        DWORD flags;
        HRESULT hr = m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) continue;

        std::vector<float> mono(frames, 0.0f);
        if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            float* s = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                float sum = 0;
                for (WORD ch = 0; ch < m_waveFormat->nChannels; ++ch)
                    sum += s[i * m_waveFormat->nChannels + ch];
                mono[i] = sum / m_waveFormat->nChannels;
            }
        } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_PCM ||
                   m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            int16_t* s = reinterpret_cast<int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                int32_t sum = 0;
                for (WORD ch = 0; ch < m_waveFormat->nChannels; ++ch)
                    sum += s[i * m_waveFormat->nChannels + ch];
                mono[i] = (float)sum / (32768.0f * m_waveFormat->nChannels);
            }
        }

        m_captureClient->ReleaseBuffer(frames);
        ProcessAudioBuffer(mono);
    }
}

void AudioVisualizerRenderer::ProcessAudioBuffer(const std::vector<float>& samples) {
    std::lock_guard lock(m_audioMutex);

    m_waveform.insert(m_waveform.end(), samples.begin(), samples.end());
    if (m_waveform.size() > 4096)
        m_waveform.erase(m_waveform.begin(), m_waveform.begin() + (m_waveform.size() - 2048));

    if (m_waveform.size() >= (size_t)FFT_SIZE) {
        std::vector<float> re(FFT_SIZE);
        std::vector<float> im(FFT_SIZE, 0.0f);

        // Apply Hann window
        auto wavEnd = m_waveform.end();
        for (int i = 0; i < FFT_SIZE; ++i) {
            float hann = 0.5f * (1.0f - cosf(6.2831853f * i / (FFT_SIZE - 1)));
            re[i] = *(wavEnd - FFT_SIZE + i) * hann;
        }

        Fft(re, im);

        m_spectrum.resize(m_numBars);
        int samplesPerBar = (FFT_SIZE / 2) / m_numBars;
        for (UINT b = 0; b < m_numBars; ++b) {
            float sum = 0;
            for (int j = 0; j < samplesPerBar; ++j) {
                int idx = b * samplesPerBar + j;
                if (idx < FFT_SIZE / 2) {
                    float mag = sqrtf(re[idx] * re[idx] + im[idx] * im[idx]);
                    sum += mag;
                }
            }
            m_spectrum[b] = (sum / samplesPerBar) * 2.0f;
            if (m_spectrum[b] > 1.0f) m_spectrum[b] = 1.0f;
        }

        m_newFrame = true;
    }
}

// ─── Rendering ────────────────────────────────────────────────────────

struct CbLayout {
    float time, resX, resY, mode;
    float numBars, pad0, pad1, pad2;
    float bars[32];
};

void AudioVisualizerRenderer::DrawBars() {
    if (!m_rootSig || !m_psoBars) return;

    std::vector<float> spectrum;
    {
        std::lock_guard lock(m_audioMutex);
        spectrum = m_spectrum;
        m_newFrame = false;
    }

    // Fill CB
    CbLayout* cb = reinterpret_cast<CbLayout*>(m_mappedSpectrum);
    cb->time = m_time;
    cb->resX = (float)m_width;
    cb->resY = (float)m_height;
    cb->mode = 0.0f;
    cb->numBars = (float)m_numBars;
    cb->pad0 = cb->pad1 = cb->pad2 = 0;
    for (UINT i = 0; i < m_numBars; ++i)
        cb->bars[i] = i < spectrum.size() ? spectrum[i] : 0;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,1};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->SetPipelineState(m_psoBars.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSig.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_uploadBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

void AudioVisualizerRenderer::DrawWaveform() {
    if (!m_rootSig || !m_psoWaveform) return;

    std::vector<float> spectrum;
    {
        std::lock_guard lock(m_audioMutex);
        spectrum = m_spectrum;
        m_newFrame = false;
    }

    CbLayout* cb = reinterpret_cast<CbLayout*>(m_mappedSpectrum);
    cb->time = m_time;
    cb->resX = (float)m_width;
    cb->resY = (float)m_height;
    cb->mode = 1.0f;
    cb->numBars = (float)m_numBars;
    cb->pad0 = cb->pad1 = cb->pad2 = 0;
    for (UINT i = 0; i < m_numBars; ++i)
        cb->bars[i] = i < spectrum.size() ? spectrum[i] : 0;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,1};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->SetPipelineState(m_psoWaveform.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSig.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_uploadBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

void AudioVisualizerRenderer::DrawParticles() {
    if (!m_rootSig || !m_psoParticles) return;

    std::vector<float> spectrum;
    {
        std::lock_guard lock(m_audioMutex);
        spectrum = m_spectrum;
        m_newFrame = false;
    }

    CbLayout* cb = reinterpret_cast<CbLayout*>(m_mappedSpectrum);
    cb->time = m_time;
    cb->resX = (float)m_width;
    cb->resY = (float)m_height;
    cb->mode = 2.0f;
    cb->numBars = (float)m_numBars;
    cb->pad0 = cb->pad1 = cb->pad2 = 0;
    for (UINT i = 0; i < m_numBars; ++i)
        cb->bars[i] = i < spectrum.size() ? spectrum[i] : 0;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,1};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->SetPipelineState(m_psoParticles.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSig.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_uploadBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

} // namespace rex::wallpaper
