#include "graphics/compositor/compositor.h"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <d3dcompiler.h>

#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED 0x087A0001
#endif

Compositor::Compositor() = default;
Compositor::~Compositor() { Shutdown(); }

bool Compositor::Initialize(HWND targetWindow) {
    RECT clientRect;
    GetClientRect(targetWindow, &clientRect);
    m_viewportWidth = static_cast<float>(clientRect.right - clientRect.left);
    m_viewportHeight = static_cast<float>(clientRect.bottom - clientRect.top);

    if (!CreateDevice()) {
        printf("[REX] Compositor: Failed to create D3D12 device\n");
        return false;
    }

    if (!CreateSrvHeap()) {
        printf("[REX] Compositor: Failed to create SRV heap\n");
        return false;
    }

    if (!CreateRootSignature()) {
        printf("[REX] Compositor: Failed to create root signature\n");
        return false;
    }

    if (!CreateVertexBuffer()) {
        printf("[REX] Compositor: Failed to create vertex buffer\n");
        return false;
    }

    if (!CreatePipelineState()) {
        printf("[REX] Compositor: Failed to create pipeline state\n");
        return false;
    }

    if (!CreateSwapChain(targetWindow)) {
        printf("[REX] Compositor: Failed to create swap chain\n");
        return false;
    }

    m_initialized = true;
    printf("[REX] Compositor initialized (D3D12) — viewport: %.0fx%.0f\n",
           m_viewportWidth, m_viewportHeight);
    return true;
}

void Compositor::Shutdown() {
    // Drain in-flight GPU work before releasing anything (audit 5.8 #3):
    // the allocator/list and render targets cannot be released while the
    // GPU may still be executing recorded commands. Bounded — a dead GPU
    // must not hang teardown, and if it is gone the release is moot anyway.
    if (m_commandQueue && m_frameFence && m_frameFenceVal > 0 && !m_deviceLost) {
        WaitForGpuFence(m_frameFence.Get(), m_frameFenceVal, m_frameFenceEvent,
                        "shutdown drain");
    }

    m_initialized = false;
    m_textures.clear();
    m_layers.clear();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_swapChain.Reset();
    for (int i = 0; i < FRAME_COUNT; i++) {
        m_renderTargets[i].Reset();
    }
    m_videoUploadBuffer.Reset();
    m_videoCopyAlloc.Reset();
    m_videoCopyList.Reset();
    m_videoCopyFence.Reset();
    if (m_videoCopyFenceEvent) {
        CloseHandle(m_videoCopyFenceEvent);
        m_videoCopyFenceEvent = nullptr;
    }
    m_videoUploadSize = 0;

    m_frameFence.Reset();
    if (m_frameFenceEvent) {
        CloseHandle(m_frameFenceEvent);
        m_frameFenceEvent = nullptr;
    }
    m_frameFenceVal = 0;
    m_deviceLost = false;
    m_backBufferStates.Reset();
}

bool Compositor::CreateDevice() {
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
    if (debugController) {
        debugController->EnableDebugLayer();
    }
#endif

    // G2: every step is HRESULT-checked — the old code ignored all of them
    // and dereferenced possibly-null objects (crash) or returned true with
    // a half-built compositor.
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        printf("[REX] Compositor: CreateDXGIFactory1 failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT ehr = factory->EnumAdapters1(i, &adapter);
        if (ehr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ehr) || !adapter) continue;

        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)))) {
            break;
        }
    }

    if (!m_device) return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr) || !m_commandQueue) {
        printf("[REX] Compositor: CreateCommandQueue failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator));
    if (FAILED(hr) || !m_commandAllocator) {
        printf("[REX] Compositor: CreateCommandAllocator failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr) || !m_commandList) {
        printf("[REX] Compositor: CreateCommandList failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_commandList->Close();
    if (FAILED(hr)) {
        printf("[REX] Compositor: command list close failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr) || !m_rtvHeap) {
        printf("[REX] Compositor: RTV heap creation failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Main-queue frame fence (audit 5.8 #3): RenderFrame waits it before
    // reusing the allocator, Shutdown drains it before releasing objects.
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                               IID_PPV_ARGS(&m_frameFence));
    if (FAILED(hr) || !m_frameFence) {
        printf("[REX] Compositor: frame fence creation failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }
    if (!m_frameFenceEvent) {
        m_frameFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_frameFenceEvent) {
            printf("[REX] Compositor: frame fence event creation failed\n");
            return false;
        }
    }

    return true;
}

// ── Failure-path helpers (audit 5.8 #3/#5/#9) ───────────────────────────────

bool Compositor::IsDeviceLostHr(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

Compositor::PresentOutcome Compositor::ClassifyPresent(HRESULT hr) {
    // DXGI_STATUS_OCCLUDED is a success code that nevertheless means the
    // frame was not displayed — distinct from a normal present.
    if (hr == DXGI_STATUS_OCCLUDED) return PresentOutcome::Occluded;
    if (SUCCEEDED(hr)) return PresentOutcome::Ok;
    if (IsDeviceLostHr(hr)) return PresentOutcome::DeviceLost;
    return PresentOutcome::Failed;
}

Compositor::GpuWaitOutcome Compositor::ClassifyGpuWait(
    unsigned long waitResult, HRESULT deviceRemovedReason) {
    if (waitResult == WAIT_OBJECT_0) return GpuWaitOutcome::Completed;
    // A wait that did not complete while the device is gone is device loss,
    // not a plain timeout — the caller can latch and stop rendering.
    if (IsDeviceLostHr(deviceRemovedReason)) return GpuWaitOutcome::DeviceLost;
    if (waitResult == WAIT_TIMEOUT) return GpuWaitOutcome::TimedOut;
    return GpuWaitOutcome::WaitFailed;
}

DXGI_FORMAT Compositor::SrvFormatForResourceFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:    return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return format;
    }
}

bool Compositor::TakeSrvSlot(std::vector<uint32_t>& freeSlots, uint32_t& nextSlot,
                             uint32_t maxSlots, uint32_t& outSlot) {
    if (!freeSlots.empty()) {
        outSlot = freeSlots.back();
        freeSlots.pop_back();
        return true;
    }
    if (nextSlot < maxSlots) {
        outSlot = nextSlot++;
        return true;
    }
    return false;
}

void Compositor::ReturnSrvSlot(std::vector<uint32_t>& freeSlots, int slot) {
    if (slot < 0) return;
    const uint32_t s = static_cast<uint32_t>(slot);
    // Guard against double returns: a slot handed out twice would alias two
    // textures onto one descriptor.
    if (std::find(freeSlots.begin(), freeSlots.end(), s) != freeSlots.end()) return;
    freeSlots.push_back(s);
}

void Compositor::MarkDeviceLost(const char* what) {
    if (m_deviceLost) return;
    m_deviceLost = true;
    const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
    printf("[REX] Compositor: rendering halted (%s); device removed reason 0x%08lx\n",
           what, static_cast<unsigned long>(reason));
}

bool Compositor::WaitForGpuFence(ID3D12Fence* fence, UINT64 value,
                                 HANDLE eventHandle, const char* what) {
    if (!fence) return true;
    if (fence->GetCompletedValue() >= value) return true;
    if (!eventHandle) {
        printf("[REX] Compositor: %s: no fence event available\n", what);
        return false;
    }

    const HRESULT hr = fence->SetEventOnCompletion(value, eventHandle);
    if (FAILED(hr)) {
        printf("[REX] Compositor: %s: SetEventOnCompletion failed: 0x%08lx\n",
               what, static_cast<unsigned long>(hr));
        if (IsDeviceLostHr(hr)) MarkDeviceLost(what);
        return false;
    }

    const DWORD result = WaitForSingleObject(eventHandle, GPU_WAIT_TIMEOUT_MS);
    const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : S_OK;
    switch (ClassifyGpuWait(result, reason)) {
        case GpuWaitOutcome::Completed:
            return true;
        case GpuWaitOutcome::DeviceLost:
            MarkDeviceLost(what);
            return false;
        case GpuWaitOutcome::TimedOut:
            // Bounded, not latched: a stalled queue may still recover; the
            // caller skips this frame instead of resetting a live allocator.
            printf("[REX] Compositor: %s: GPU wait timed out after %lu ms "
                   "(skipping this frame)\n", what, GPU_WAIT_TIMEOUT_MS);
            return false;
        case GpuWaitOutcome::WaitFailed:
            printf("[REX] Compositor: %s: WaitForSingleObject failed (0x%08lx)\n",
                   what, static_cast<unsigned long>(result));
            return false;
    }
    return false;
}

void Compositor::RecycleTextureSlot(uint64_t layerId) {
    auto it = m_textures.find(layerId);
    if (it != m_textures.end()) {
        ReturnSrvSlot(m_freeSrvSlots, it->second.srvSlot);
    }
}

bool Compositor::CreateSrvHeap() {
    if (!m_device) return false;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = MAX_TEXTURES;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = m_device->CreateDescriptorHeap(&srvHeapDesc,
                                                IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr) || !m_srvHeap) {
        printf("[REX] Compositor: SRV heap creation failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Compositor::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &signature, &error);
    if (FAILED(hr)) {
        printf("[REX] Failed to serialize root signature: %s\n",
            error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown");
        return false;
    }

    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) {
        printf("[REX] Failed to create root signature\n");
        return false;
    }

    return true;
}

bool Compositor::CreateVertexBuffer() {
    struct Vertex { float x, y, u, v; };
    Vertex vertices[] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
    };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc = {};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = sizeof(vertices);
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vbDesc.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vertexBuffer)))) {
        return false;
    }

    void* mapped;
    m_vertexBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, vertices, sizeof(vertices));
    m_vertexBuffer->Unmap(0, nullptr);

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    m_vertexBufferView.SizeInBytes = sizeof(vertices);

    uint32_t indices[] = { 0, 1, 2, 1, 3, 2 };
    m_indexCount = 6;

    D3D12_RESOURCE_DESC ibDesc = {};
    ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ibDesc.Width = sizeof(indices);
    ibDesc.Height = 1;
    ibDesc.DepthOrArraySize = 1;
    ibDesc.MipLevels = 1;
    ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ibDesc.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_indexBuffer)))) {
        return false;
    }

    m_indexBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, indices, sizeof(indices));
    m_indexBuffer->Unmap(0, nullptr);

    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = sizeof(indices);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

bool Compositor::CreatePipelineState() {
    const char* vsSrc =
        "struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };"
        "struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };"
        "VSOutput main(VSInput input) {"
        "    VSOutput output;"
        "    output.pos = float4(input.pos, 0.0, 1.0);"
        "    output.uv = input.uv;"
        "    return output;"
        "}";

    const char* psSrc =
        "struct PSInput {"
        "    float4 pos : SV_POSITION;"
        "    float2 uv : TEXCOORD;"
        "};"
        "Texture2D tex : register(t0);"
        "SamplerState sam : register(s0);"
        "float4 main(PSInput input) : SV_TARGET {"
        "    float4 color = tex.Sample(sam, input.uv);"
        "    color.a = 1.0;"
        "    return color;"
        "}";

    ComPtr<ID3DBlob> vs, ps, error;

    if (FAILED(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
                          "main", "vs_5_0", 0, 0, &vs, &error))) {
        printf("[REX] Failed to compile vertex shader: %s\n",
               error ? (const char*)error->GetBufferPointer() : "unknown");
        return false;
    }
    if (FAILED(D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
                          "main", "ps_5_0", 0, 0, &ps, &error))) {
        printf("[REX] Failed to compile pixel shader: %s\n",
               error ? (const char*)error->GetBufferPointer() : "unknown");
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, 2 };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&m_pipelineState)))) {
        printf("[REX] Failed to create pipeline state\n");
        return false;
    }

    return true;
}

bool Compositor::CreateSwapChain(HWND targetWindow) {
    // G2: HRESULT-checked end to end (was unchecked → null deref on any
    // failure, and true returned with a half-built swapchain).
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        printf("[REX] Compositor: CreateDXGIFactory1 failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    RECT clientRect;
    GetClientRect(targetWindow, &clientRect);
    UINT width = clientRect.right - clientRect.left;
    UINT height = clientRect.bottom - clientRect.top;

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = std::max(width, 1U);
    swapChainDesc.Height = std::max(height, 1U);
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    hr = factory->CreateSwapChainForHwnd(m_commandQueue.Get(), targetWindow,
        &swapChainDesc, nullptr, nullptr, &tempSwapChain);
    if (FAILED(hr) || !tempSwapChain) {
        printf("[REX] Compositor: CreateSwapChainForHwnd failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    hr = tempSwapChain.As(&m_swapChain);
    if (FAILED(hr) || !m_swapChain) {
        printf("[REX] Compositor: swapchain QI failed: 0x%08lx\n",
               static_cast<unsigned long>(hr));
        return false;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    for (int i = 0; i < FRAME_COUNT; i++) {
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr) || !m_renderTargets[i]) {
            printf("[REX] Compositor: GetBuffer(%d) failed: 0x%08lx\n", i,
                   static_cast<unsigned long>(hr));
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += i * m_rtvDescriptorSize;
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
    }

    // Fresh swap-chain buffers begin in PRESENT.
    m_backBufferStates.Reset();

    return true;
}

void Compositor::PopulateCommandList() {
    // Transition the back buffer from its TRACKED state → RENDER_TARGET
    // (audit 5.8 #9): hardcoding PRESENT as StateBefore was wrong for a
    // buffer whose previous frame/Present did not complete normally.
    const D3D12_RESOURCE_STATES before = m_backBufferStates.Before(m_frameIndex);
    if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);
    }
    m_backBufferStates.OnRenderBegin(m_frameIndex);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_frameIndex * m_rtvDescriptorSize;

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtvHandle, m_clearColor, 0, nullptr);

    D3D12_VIEWPORT fullViewport = {
        0.0f, 0.0f, m_viewportWidth, m_viewportHeight, 0.0f, 1.0f
    };
    D3D12_RECT fullScissor = {
        0, 0,
        static_cast<LONG>(m_viewportWidth),
        static_cast<LONG>(m_viewportHeight)
    };
    m_commandList->RSSetViewports(1, &fullViewport);
    m_commandList->RSSetScissorRects(1, &fullScissor);

    // Draw compositor layers when present. When no layers are registered,
    // the ClearRenderTargetView above already fills the back buffer
    // with the clear color — no draw call is needed. DWM will
    // composite the cleared buffer as-is.
    if (!m_layers.empty()) {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_pipelineState.Get());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);

        m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        m_commandList->IASetIndexBuffer(&m_indexBufferView);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto sortedLayers = m_layers;
        std::sort(sortedLayers.begin(), sortedLayers.end(),
            [](const Layer& a, const Layer& b) { return a.zOrder < b.zOrder; });

        for (const auto& layer : sortedLayers) {
            if (!layer.visible) continue;

            auto texIt = m_textures.find(layer.id);
            if (texIt == m_textures.end()) continue;

            D3D12_VIEWPORT layerVp = {
                layer.bounds[0], layer.bounds[1],
                layer.bounds[2], layer.bounds[3],
                0.0f, 1.0f
            };
            m_commandList->RSSetViewports(1, &layerVp);

            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
                m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            gpuHandle.ptr += static_cast<UINT64>(texIt->second.srvSlot)
                          * m_srvDescriptorSize;
            m_commandList->SetGraphicsRootDescriptorTable(0, gpuHandle);

            m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
        }
    }

    // Transition back buffer from RENDER_TARGET → PRESENT
    D3D12_RESOURCE_BARRIER exitBarrier = {};
    exitBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    exitBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    exitBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    exitBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    exitBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &exitBarrier);
    m_backBufferStates.OnRenderEnd(m_frameIndex);
}

void Compositor::RenderFrame() {
    if (!m_initialized || m_deviceLost) return;
    static_assert(BackBufferStateTracker::COUNT == FRAME_COUNT,
                  "back-buffer tracker must cover every swap-chain buffer");

    // The allocator and command list cannot be reset while the previous
    // frame is still executing on the GPU (audit 5.8 #3): wait, bounded,
    // for the frame fence first. A timed-out wait skips the frame instead
    // of resetting a live allocator; a lost device latches rendering off.
    if (m_frameFenceVal > 0 &&
        !WaitForGpuFence(m_frameFence.Get(), m_frameFenceVal,
                         m_frameFenceEvent, "frame fence")) {
        return;
    }

    HRESULT hr = m_commandAllocator->Reset();
    if (FAILED(hr)) {
        if (IsDeviceLostHr(hr)) {
            MarkDeviceLost("allocator reset");
        } else {
            printf("[REX] Compositor: allocator reset failed: 0x%08lx\n",
                   static_cast<unsigned long>(hr));
        }
        return;
    }

    hr = m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());
    if (FAILED(hr)) {
        if (IsDeviceLostHr(hr)) {
            MarkDeviceLost("command list reset");
        } else {
            printf("[REX] Compositor: command list reset failed: 0x%08lx\n",
                   static_cast<unsigned long>(hr));
        }
        return;
    }

    PopulateCommandList();

    hr = m_commandList->Close();
    if (FAILED(hr)) {
        if (IsDeviceLostHr(hr)) {
            MarkDeviceLost("command list close");
        } else {
            printf("[REX] Compositor: command list close failed: 0x%08lx\n",
                   static_cast<unsigned long>(hr));
        }
        return;
    }

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    // Signal AFTER submission (audit 5.8 #3): the next frame's reuse wait
    // covers every command this frame recorded.
    m_frameFenceVal++;
    m_commandQueue->Signal(m_frameFence.Get(), m_frameFenceVal);

    hr = m_swapChain->Present(1, 0);

    switch (ClassifyPresent(hr)) {
        case PresentOutcome::Ok:
            break;
        case PresentOutcome::Occluded: {
            static int occludedCount = 0;
            occludedCount++;
            if (occludedCount % 300 == 1) {
                printf("[REX] Compositor: DXGI_STATUS_OCCLUDED "
                       "(count=%d). DWM considers the window occluded.\n",
                       occludedCount);
            }
            break;
        }
        case PresentOutcome::DeviceLost:
            // The swap chain's state is unknowable after device loss; stop
            // touching it (audit 5.8 #3/#9) until a fresh Initialize().
            MarkDeviceLost("Present");
            return;
        case PresentOutcome::Failed:
            printf("[REX] Compositor: Present FAILED 0x%08lx\n",
                   static_cast<unsigned long>(hr));
            break;
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Compositor::SubmitLayer(const Layer& layer) {
    auto it = std::find_if(m_layers.begin(), m_layers.end(),
        [&](const Layer& l) { return l.id == layer.id; });
    if (it != m_layers.end()) {
        *it = layer;
    } else {
        m_layers.push_back(layer);
    }
}

void Compositor::RemoveLayer(uint64_t id) {
    std::erase_if(m_layers, [id](const Layer& l) { return l.id == id; });
}

void Compositor::SetClearColor(float r, float g, float b) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = 1.0f;
}

bool Compositor::ImportLayerTexture(
    uint64_t layerId, HANDLE sharedHandle, int width, int height)
{
    if (!m_device || !sharedHandle) return false;

    // Open and validate BEFORE consuming an SRV slot (audit 5.8 #7): derive
    // the view format and dimensions from the resource itself instead of
    // forcing R8G8B8A8 and trusting caller numbers.
    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&resource));
    if (FAILED(hr) || !resource) {
        printf("[REX] Compositor: OpenSharedHandle failed (0x%08lx)\n",
               static_cast<unsigned long>(hr));
        return false;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || desc.MipLevels < 1) {
        printf("[REX] Compositor: shared handle is not a single-sample 2D "
               "texture (dim=%d samples=%u mips=%u)\n",
               static_cast<int>(desc.Dimension), desc.SampleDesc.Count,
               desc.MipLevels);
        return false;
    }
    const DXGI_FORMAT srvFormat = SrvFormatForResourceFormat(desc.Format);
    if (srvFormat == DXGI_FORMAT_UNKNOWN) {
        printf("[REX] Compositor: shared texture has an unusable format (%d)\n",
               static_cast<int>(desc.Format));
        return false;
    }
    if (width > 0 && height > 0 &&
        (desc.Width != static_cast<UINT64>(width) ||
         desc.Height != static_cast<UINT>(height))) {
        printf("[REX] Compositor: caller size %dx%d != shared resource "
               "%llux%u — using the resource\n",
               width, height, static_cast<unsigned long long>(desc.Width),
               desc.Height);
    }

    // Replacing an existing layer must recycle its SRV slot first
    // (audit 5.8 #6) or every replace leaks one until the 64-slot heap
    // runs out. Taking the new slot right after also reuses it.
    RecycleTextureSlot(layerId);

    uint32_t slot = 0;
    if (!TakeSrvSlot(m_freeSrvSlots, m_nextSrvSlot, MAX_TEXTURES, slot)) {
        printf("[REX] Compositor: out of SRV slots\n");
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srvFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<UINT64>(slot) * m_srvDescriptorSize;
    m_device->CreateShaderResourceView(resource.Get(), &srvDesc, cpuHandle);

    LayerTexture lt;
    lt.resource = std::move(resource);
    lt.srvSlot = static_cast<int>(slot);
    lt.texWidth = static_cast<int>(desc.Width);
    lt.texHeight = static_cast<int>(desc.Height);
    m_textures[layerId] = std::move(lt);

    printf("[REX] Compositor: imported texture for layer %llu "
           "(slot %u, %llux%u fmt=%d)\n",
           layerId, slot, static_cast<unsigned long long>(desc.Width),
           desc.Height, static_cast<int>(srvFormat));
    return true;
}

void Compositor::RemoveLayerTexture(uint64_t layerId) {
    auto it = m_textures.find(layerId);
    if (it == m_textures.end()) return;

    ReturnSrvSlot(m_freeSrvSlots, it->second.srvSlot);
    m_textures.erase(it);
    printf("[REX] Compositor: removed texture for layer %llu\n", layerId);
}

bool Compositor::UpdateLayerPixels(uint64_t layerId,
                                   const uint8_t* pixels,
                                   int width, int height, int rowPitch) {
    if (!m_device) return false;

    auto texIt = m_textures.find(layerId);
    if (texIt == m_textures.end()) {
        uint32_t slot = 0;
        if (!TakeSrvSlot(m_freeSrvSlots, m_nextSrvSlot, MAX_TEXTURES, slot)) {
            return false;
        }
        LayerTexture placeholder;
        placeholder.srvSlot = static_cast<int>(slot);
        auto result = m_textures.emplace(layerId, std::move(placeholder));
        texIt = result.first;
    }
    LayerTexture& lt = texIt->second;

    // Recreate target texture on size change
    bool sizeChanged = (!lt.resource || lt.texWidth != width || lt.texHeight != height);
    if (sizeChanged) {
        if (lt.resource) lt.resource.Reset();
        m_videoUploadBuffer.Reset();

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = static_cast<UINT64>(width);
        texDesc.Height           = static_cast<UINT>(height);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&lt.resource));
        if (FAILED(hr)) return false;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<UINT64>(lt.srvSlot) * m_srvDescriptorSize;
        m_device->CreateShaderResourceView(lt.resource.Get(), &srvDesc, cpuHandle);

        lt.texWidth  = width;
        lt.texHeight = height;
        lt.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    // Ensure upload buffer is large enough
    UINT64 uploadSize = static_cast<UINT64>(rowPitch) * height;
    uploadSize = (uploadSize + 511) & ~511ULL;

    if (!m_videoUploadBuffer || uploadSize > m_videoUploadSize) {
        m_videoUploadBuffer.Reset();
        m_videoUploadSize = static_cast<uint32_t>(uploadSize);

        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width            = uploadSize;
        uploadDesc.Height           = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels        = 1;
        uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
        uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.SampleDesc.Count = 1;

        HRESULT hr = m_device->CreateCommittedResource(
            &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_videoUploadBuffer));
        if (FAILED(hr)) return false;
    }

    // Map and copy pixels
    void* mapped = nullptr;
    HRESULT hr = m_videoUploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) return false;
    {
        const uint8_t* src = pixels;
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < height; ++row) {
            memcpy(dst, src, static_cast<size_t>(rowPitch));
            src += rowPitch;
            dst += rowPitch;
        }
    }
    m_videoUploadBuffer->Unmap(0, nullptr);

    // Ensure copy resources exist
    if (!m_videoCopyAlloc) {
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&m_videoCopyAlloc));
        if (FAILED(hr)) return false;
    }
    if (!m_videoCopyList) {
        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          m_videoCopyAlloc.Get(), nullptr,
                                          IID_PPV_ARGS(&m_videoCopyList));
        if (FAILED(hr)) return false;
    }
    if (!m_videoCopyFence) {
        hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&m_videoCopyFence));
        if (FAILED(hr)) return false;
    }
    if (!m_videoCopyFenceEvent) {
        m_videoCopyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_videoCopyFenceEvent) return false;
    }

    // Reset and record copy commands
    m_videoCopyAlloc->Reset();
    m_videoCopyList->Reset(m_videoCopyAlloc.Get(), nullptr);

    // Barrier bookkeeping (audit 5.8 #4): the texture is COPY_DEST only
    // right after creation; a second call finds it PIXEL_SHADER_RESOURCE and
    // claiming COPY_DEST as StateBefore was a barrier violation.
    if (lt.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER toCopy = {};
        toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = lt.resource.Get();
        toCopy.Transition.StateBefore = lt.state;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_videoCopyList->ResourceBarrier(1, &toCopy);
        lt.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource        = lt.resource.Get();
        dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = m_videoUploadBuffer.Get();
        srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = 0;
        srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(width);
        srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(height);
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        m_videoCopyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = lt.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_videoCopyList->ResourceBarrier(1, &barrier);
        lt.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    m_videoCopyList->Close();

    ID3D12CommandList* lists[] = { m_videoCopyList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Wait for copy to complete — bounded (audit 5.8 #5): the old INFINITE
    // wait froze the compositor with no device-removed escape.
    m_videoCopyFenceVal++;
    m_commandQueue->Signal(m_videoCopyFence.Get(), m_videoCopyFenceVal);
    if (!WaitForGpuFence(m_videoCopyFence.Get(), m_videoCopyFenceVal,
                         m_videoCopyFenceEvent, "layer pixel copy")) {
        return false;
    }

    return true;
}

// Debug tracing for the file-background path. Gated behind REX_BG_LOG
// (audit 5.8 #11): it used to append to a hardcoded developer path
// (C:\Users\munna\...) that exists on no other machine, unconditionally, on
// every call. Enabled runs write to %TEMP%\Paiint\paiint_bg_log.txt.
static void LogToFile(const char* msg) {
    static const bool enabled = std::getenv("REX_BG_LOG") != nullptr;
    if (!enabled) return;
    char tmp[MAX_PATH] = {};
    const DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return;
    std::string path(tmp);
    path += "Paiint";
    CreateDirectoryA(path.c_str(), nullptr);
    path += "\\paiint_bg_log.txt";
    if (FILE* f = fopen(path.c_str(), "a")) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

bool Compositor::SetBackgroundFromFile(const char* filePath) {
    LogToFile("SetBackgroundFromFile entered");
    if (!m_device) { LogToFile("  FAIL: no device"); return false; }

    LogToFile("  Decoding image...");
    auto texData = rex::wallpaper::DecodeTextureFromFile(filePath);
    if (texData.pixels.empty()) {
        char buf[512]; snprintf(buf, sizeof(buf), "  FAIL: decode failed for '%s'", filePath);
        LogToFile(buf);
        return false;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "  Decoded OK: %dx%d, rowPitch=%d",
             texData.width, texData.height, texData.rowPitch);
    LogToFile(buf);

    uint32_t slot = 0;
    if (!TakeSrvSlot(m_freeSrvSlots, m_nextSrvSlot, MAX_TEXTURES, slot)) {
        LogToFile("  FAIL: out of SRV slots");
        return false;
    }
    snprintf(buf, sizeof(buf), "  Allocated SRV slot %d", static_cast<int>(slot));
    LogToFile(buf);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = static_cast<UINT64>(texData.width);
    texDesc.Height           = static_cast<UINT>(texData.height);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> texture;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        LogToFile("  FAIL: CreateCommittedResource texture");
        ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false;
    }
    LogToFile("  Created texture OK");

    UINT64 uploadSize = static_cast<UINT64>(texData.rowPitch) * texData.height;
    uploadSize = (uploadSize + 511) & ~511ULL;

    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> uploadBuffer;
    hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr)) {
        LogToFile("  FAIL: upload buffer creation");
        ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false;
    }
    LogToFile("  Created upload buffer OK");

    void* mapped = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) { LogToFile("  FAIL: Map upload buffer"); ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false; }
    {
        const uint8_t* src = texData.pixels.data();
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < texData.height; ++row) {
            memcpy(dst, src, static_cast<size_t>(texData.rowPitch));
            src += texData.rowPitch;
            dst += texData.rowPitch;
        }
    }
    uploadBuffer->Unmap(0, nullptr);
    LogToFile("  Copied pixels to upload buffer");

    ComPtr<ID3D12CommandAllocator> copyAlloc;
    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&copyAlloc));
    if (FAILED(hr)) { LogToFile("  FAIL: create copy allocator"); ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false; }

    ComPtr<ID3D12GraphicsCommandList> copyList;
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      copyAlloc.Get(), nullptr,
                                      IID_PPV_ARGS(&copyList));
    if (FAILED(hr)) { LogToFile("  FAIL: create copy list"); ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false; }
    LogToFile("  Created copy list OK");

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = texture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(texData.width);
    srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(texData.height);
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(texData.rowPitch);

    copyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copyList->ResourceBarrier(1, &barrier);

    copyList->Close();

    ID3D12CommandList* lists[] = { copyList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Wait for copy to complete
    ComPtr<ID3D12Fence> fence;
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) { LogToFile("  FAIL: create fence"); ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false; }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) { LogToFile("  FAIL: create event"); ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot)); return false; }

    UINT64 fenceVal = 1;
    m_commandQueue->Signal(fence.Get(), fenceVal);
    if (!WaitForGpuFence(fence.Get(), fenceVal, fenceEvent, "background upload")) {
        CloseHandle(fenceEvent);
        ReturnSrvSlot(m_freeSrvSlots, static_cast<int>(slot));
        return false;
    }
    CloseHandle(fenceEvent);
    LogToFile("  Copy completed via fence");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<UINT64>(slot) * m_srvDescriptorSize;
    m_device->CreateShaderResourceView(texture.Get(), &srvDesc, cpuHandle);
    LogToFile("  Created SRV");

    LayerTexture lt;
    lt.resource = std::move(texture);
    lt.srvSlot = slot;
    lt.texWidth = texData.width;
    lt.texHeight = texData.height;
    constexpr uint64_t BG_LAYER_ID = 1;
    // Recycle any slot a previous background image held (audit 5.8 #6):
    // overwriting the entry used to leak its descriptor until the heap ran out.
    RecycleTextureSlot(BG_LAYER_ID);
    m_textures[BG_LAYER_ID] = std::move(lt);

    Layer bgLayer;
    bgLayer.id = BG_LAYER_ID;
    bgLayer.kind = 0;
    bgLayer.bounds[0] = 0.0f;
    bgLayer.bounds[1] = 0.0f;
    bgLayer.bounds[2] = m_viewportWidth;
    bgLayer.bounds[3] = m_viewportHeight;
    bgLayer.opacity = 1.0f;
    bgLayer.zOrder = 0;
    bgLayer.visible = true;
    SubmitLayer(bgLayer);
    LogToFile("  Submitted layer OK");

    snprintf(buf, sizeof(buf), "SetBackgroundFromFile SUCCESS: %s (%dx%d)",
             filePath, texData.width, texData.height);
    LogToFile(buf);
    return true;
}
