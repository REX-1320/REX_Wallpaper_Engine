#include "D3D12Renderer.hpp"
#include "../decoder/WICTextureLoader.hpp"
#include <shared/Logger.hpp>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace rex::wallpaper {

// Shader Sources
static const char* g_vertexShaderSource = R"(
struct VSInput {
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
};
struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};
VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}
)";

static const char* g_pixelShaderSource = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);
struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};
float4 main(VSOutput input) : SV_TARGET {
    return tex.Sample(sam, input.uv);
}
)";

// Constructor / Destructor
D3D12Renderer::D3D12Renderer() = default;
D3D12Renderer::~D3D12Renderer() { Shutdown(); }

// Initialize
shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height) {
    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized, "Renderer already initialized");
    }

    REX_LOG_INFO("D3D12", "Initializing D3D12 renderer ({}x{})", width, height);

    m_width = width;
    m_height = height;

    // Create D3D12 Device
    if (!CreateDevice()) {
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported, "Failed to create D3D12 device");
    }

    // Create Command Queue
    if (!CreateCommandQueue()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create command queue");
    }

    // Create Command Allocator
    if (!CreateCommandAllocator()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create command allocator");
    }

    // Create Command List
    if (!CreateCommandList()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create command list");
    }

    // Create Fence
    if (!CreateFence()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create fence");
    }

    // Create Swap Chain
    if (!CreateSwapChain(targetWindow)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create swap chain");
    }

    // Get render target buffers
    if (!CreateRenderTargetViews()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create render target views");
    }

    // Create Descriptor Heaps
    if (!CreateDescriptorHeaps()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create descriptor heaps");
    }

    // Create Root Signature
    if (!CreateRootSignature()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create root signature");
    }

    // Create Vertex Buffer
    if (!CreateVertexBuffer()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create vertex buffer");
    }

    // Create Pipeline State
    if (!CreatePipelineState()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Failed to create pipeline state");
    }

    m_initialized = true;
    REX_LOG_INFO("D3D12", "D3D12 renderer initialized successfully");
    return shared::Result<void>();
}

// Device
bool D3D12Renderer::CreateDevice() {
    // ── Enable debug layer in debug builds ──────────────────────────────
#ifdef _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            REX_LOG_INFO("D3D12", "Debug layer enabled");
        }
    }
#endif

    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "D3D12CreateDevice failed: {:08x}", hr);
        return false;
    }

    D3D12_FEATURE_DATA_FEATURE_LEVELS featLevels = {};
    D3D_FEATURE_LEVEL levelsRequested[] = { D3D_FEATURE_LEVEL_11_0 };
    featLevels.NumFeatureLevels = 1;
    featLevels.pFeatureLevelsRequested = levelsRequested;
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featLevels, sizeof(featLevels)))) {
        REX_LOG_DEBUG("D3D12", "D3D12 device created (feature level: {}.{})",
                      (featLevels.MaxSupportedFeatureLevel >> 12) & 0xF,
                      (featLevels.MaxSupportedFeatureLevel >> 8) & 0xF);
    }

    // ── Set device name for debugging ──────────────────────────────────
    m_device->SetName(L"REX D3D12 Device");
    return true;
}

// Command Queue
bool D3D12Renderer::CreateCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommandQueue failed: {:08x}", hr);
        return false;
    }
    return true;
}

// Command Allocator
bool D3D12Renderer::CreateCommandAllocator() {
    HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&m_commandAllocator));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommandAllocator failed: {:08x}", hr);
        return false;
    }
    return true;
}

// Command List
bool D3D12Renderer::CreateCommandList() {
    HRESULT hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              m_commandAllocator.Get(), nullptr,
                                              IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommandList failed: {:08x}", hr);
        return false;
    }
    m_commandList->Close();
    return true;
}

// Fence
bool D3D12Renderer::CreateFence() {
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateFence failed: {:08x}", hr);
        return false;
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        REX_LOG_ERROR("D3D12", "CreateEvent failed: {}", GetLastError());
        return false;
    }

    return true;
}

// Swap Chain
bool D3D12Renderer::CreateSwapChain(HWND targetWindow) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateDXGIFactory2 failed: {:08x}", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount = 3;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(m_commandQueue.Get(), targetWindow, &desc,
                                         nullptr, nullptr, &swapChain1);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateSwapChainForHwnd failed: {:08x}", hr);
        return false;
    }

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Failed to get IDXGISwapChain4: {:08x}", hr);
        return false;
    }

    // Set max frame latency to 2 for smooth vsync-aligned pacing
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain1.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(2);
        m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
        REX_LOG_INFO("D3D12", "Frame latency waitable object created");
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Disable automatic Alt+Enter handling
    Microsoft::WRL::ComPtr<IDXGIFactory> parentFactory;
    if (SUCCEEDED(m_swapChain->GetParent(IID_PPV_ARGS(&parentFactory)))) {
        parentFactory->MakeWindowAssociation(targetWindow,
            DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }

    return true;
}

// Render Target Views (acquire buffers from swap chain)
bool D3D12Renderer::CreateRenderTargetViews() {
    for (int i = 0; i < 3; i++) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            REX_LOG_ERROR("D3D12", "GetBuffer {} failed: {:08x}", i, hr);
            return false;
        }
    }
    return true;
}

// Descriptor Heaps
bool D3D12Renderer::CreateDescriptorHeaps() {
    // RTV Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 3;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    HRESULT hr = m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateDescriptorHeap (RTV) failed: {:08x}", hr);
        return false;
    }

    // Create RTVs
    UINT rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < 3; i++) {
        m_rtvHandles[i] = handle;
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += rtvSize;
    }

    // SRV Heap
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = 64;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateDescriptorHeap (SRV) failed: {:08x}", hr);
        return false;
    }

    return true;
}

// Root Signature
bool D3D12Renderer::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;

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
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
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

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "SerializeRootSignature failed: {:08x}", hr);
        return false;
    }

    hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                       sigBlob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateRootSignature failed: {:08x}", hr);
        return false;
    }

    return true;
}

// Vertex Buffer
bool D3D12Renderer::CreateVertexBuffer() {
    Vertex vertices[] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f}
    };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = sizeof(vertices);
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
                                                   &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&m_vertexBuffer));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateVertexBuffer failed: {:08x}", hr);
        return false;
    }

    void* mapped = nullptr;
    hr = m_vertexBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Map vertex buffer failed: {:08x}", hr);
        return false;
    }
    memcpy(mapped, vertices, sizeof(vertices));
    m_vertexBuffer->Unmap(0, nullptr);

    return true;
}

// Pipeline State
bool D3D12Renderer::CreatePipelineState() {
    // Compile shaders
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompile(g_vertexShaderSource, strlen(g_vertexShaderSource),
                            nullptr, nullptr, nullptr, "main", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Vertex shader compile failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "");
        return false;
    }

    hr = D3DCompile(g_pixelShaderSource, strlen(g_pixelShaderSource),
                    nullptr, nullptr, nullptr, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &psBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Pixel shader compile failed: {}",
                      errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "");
        return false;
    }

    // Input layout
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.InputLayout = {inputLayout, 2};
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
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateGraphicsPipelineState failed: {:08x}", hr);
        return false;
    }

    return true;
}

// Wait for GPU
void D3D12Renderer::WaitForGPU() {
    m_fenceValue++;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), m_fenceValue);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Signal failed: {:08x}", hr);
        return;
    }

    if (m_fence->GetCompletedValue() < m_fenceValue) {
        hr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
}

// Shutdown
void D3D12Renderer::Shutdown() {
    if (!m_initialized) return;

    REX_LOG_INFO("D3D12", "Shutting down renderer...");

    std::lock_guard<std::mutex> lock(m_renderMutex);

    if (m_commandQueue) {
        WaitForGPU();
    }

    m_texture.Reset();
    m_hasTexture = false;

    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_srvHeap.Reset();
    m_rtvHeap.Reset();
    for (auto& rt : m_renderTargets) rt.Reset();
    m_swapChain.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_fence.Reset();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }

    m_initialized = false;
    REX_LOG_INFO("D3D12", "Renderer shutdown complete");
}

// Render Loop
void D3D12Renderer::WaitForFrame() {
    // Wait for the previous frame to complete.
    // This provides vsync-aligned frame pacing without busy-waiting.
    // The waitable object is signaled when the GPU has consumed the frame.
    if (m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, FALSE);
    }
}

void D3D12Renderer::BeginFrame() {
    // [Stage 6] 1Hz diagnostic
    {
        static auto s_nextLog = std::chrono::steady_clock::now();
        auto s_now = std::chrono::steady_clock::now();
        if (s_now >= s_nextLog) {
            s_nextLog = s_now + std::chrono::seconds(1);
            REX_LOG_INFO("GfxDiag", "BeginFrame: frameIdx={}", m_frameIndex);
        }
    }

    m_renderMutex.lock();

    WaitForFrame();

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());
}

void D3D12Renderer::RenderFrame() {
    // [Stage 5] 1Hz diagnostic: RenderFrame entry
    {
        static auto s_nextLog = std::chrono::steady_clock::now();
        auto s_now = std::chrono::steady_clock::now();
        if (s_now >= s_nextLog) {
            s_nextLog = s_now + std::chrono::seconds(1);
            REX_LOG_INFO("GfxDiag", "RenderFrame: hasTex={} tex={} srv=0x{:016x} texState={}",
                         m_hasTexture ? 1 : 0,
                         fmt::ptr(m_texture.Get()),
                         m_textureSRVGpuHandle.ptr,
                         static_cast<int>(m_textureState));
        }
    }

    // Transition back buffer to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    // Set render target
    m_commandList->OMSetRenderTargets(1, &m_rtvHandles[m_frameIndex], FALSE, nullptr);

    // Clear to background color (visible when no texture is loaded)
    m_commandList->ClearRenderTargetView(m_rtvHandles[m_frameIndex], m_clearColor, 0, nullptr);

    // Set viewport and scissor (adjusted for fill mode)
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    ComputeFillViewport(viewport, scissor);
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    // Draw textured fullscreen quad
    if (m_hasTexture) {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_pipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = sizeof(Vertex) * 4;
        vbv.StrideInBytes = sizeof(Vertex);
        m_commandList->IASetVertexBuffers(0, 1, &vbv);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetGraphicsRootDescriptorTable(0, m_textureSRVGpuHandle);

        m_commandList->DrawInstanced(4, 1, 0, 0);

        // [Stage 5] 1Hz diagnostic: DrawInstanced
        {
            static auto s_nextLog = std::chrono::steady_clock::now();
            auto s_now = std::chrono::steady_clock::now();
            if (s_now >= s_nextLog) {
                s_nextLog = s_now + std::chrono::seconds(1);
                REX_LOG_INFO("GfxDiag", "DrawInstanced: hasTex={} drawn=1", m_hasTexture ? 1 : 0);
            }
        }
    }

    // Transition back buffer to present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Renderer::EndFrame() {
    m_commandList->Close();

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);

    HRESULT hr = m_swapChain->Present(1, 0);

    // [Stage 6] 1Hz diagnostic: Present result
    {
        static auto s_nextLog = std::chrono::steady_clock::now();
        auto s_now = std::chrono::steady_clock::now();
        if (s_now >= s_nextLog) {
            s_nextLog = s_now + std::chrono::seconds(1);
            REX_LOG_INFO("GfxDiag", "Present: hr={:08x} frameIdx={}", hr, m_frameIndex);
        }
    }

    // Detect device removal (TDR, driver crash, etc.)
    if (FAILED(hr)) {
        HRESULT deviceRemovedReason = m_device->GetDeviceRemovedReason();
        if (deviceRemovedReason == DXGI_ERROR_DEVICE_REMOVED ||
            deviceRemovedReason == DXGI_ERROR_DEVICE_RESET ||
            deviceRemovedReason == DXGI_ERROR_DEVICE_HUNG ||
            deviceRemovedReason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
            REX_LOG_ERROR("D3D12", "Device removed: {:08x}", deviceRemovedReason);
            m_deviceRemoved = true;
        }
    }

    // Signal fence
    m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);

    m_renderMutex.unlock();
}

void D3D12Renderer::SetClearColor(float r, float g, float b) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = 1.0f;
}

void D3D12Renderer::Resize(int width, int height) {
    if (width == m_width && height == m_height) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);

    REX_LOG_INFO("D3D12", "Resizing to {}x{}", width, height);

    WaitForGPU();

    for (auto& rt : m_renderTargets) rt.Reset();

    m_width = width;
    m_height = height;

    HRESULT hr = m_swapChain->ResizeBuffers(3, m_width, m_height,
                                            DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "ResizeBuffers failed: {:08x}", hr);
        return;
    }

    CreateRenderTargetViews();
}

// ─── Texture Loading ────────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::LoadTexture(std::string_view path) {
    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }

    REX_LOG_INFO("D3D12", "Loading texture: {}", path);

    // Decode image via WIC
    TextureData texData = DecodeTextureFromFile(path);
    if (texData.pixels.empty()) {
        REX_LOG_ERROR("D3D12", "Failed to decode texture: {}", path);
        return shared::Result<void>(shared::ErrorCode::InvalidFormat,
                                    "WIC decode failed: " + std::string(path));
    }

    // Upload to GPU
    std::lock_guard<std::mutex> lock(m_renderMutex);

    // Release old texture first
    if (m_texture) {
        WaitForGPU();
        m_texture.Reset();
        m_hasTexture = false;
    }

    if (!UploadTextureToGPU(texData)) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "GPU texture upload failed");
    }

    REX_LOG_INFO("D3D12", "Texture loaded: {}x{}", texData.width, texData.height);
    return shared::Result<void>();
}

void D3D12Renderer::ClearTexture() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (m_texture) {
        WaitForGPU();
        m_texture.Reset();
        m_textureUploadBuffer.Reset();
        m_hasTexture = false;
        REX_LOG_INFO("D3D12", "Texture cleared");
    }
}

// ─── Update Texture Pixels (for video frames) ─────────────────────────────

namespace {
    // Check whether a D3D12 HRESULT failure indicates device loss.
    bool IsDeviceRemovedHr(HRESULT hr) {
        return hr == DXGI_ERROR_DEVICE_REMOVED ||
               hr == DXGI_ERROR_DEVICE_RESET ||
               hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
}

shared::Result<void> D3D12Renderer::UpdateTexturePixels(
    const uint8_t* pixels, int width, int height, int rowPitch) {

    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }
    if (!pixels || width <= 0 || height <= 0) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Invalid pixel data");
    }

    // [Stage 4] 1Hz diagnostic: entry
    {
        static auto s_nextLog = std::chrono::steady_clock::now();
        auto s_now = std::chrono::steady_clock::now();
        if (s_now >= s_nextLog) {
            s_nextLog = s_now + std::chrono::seconds(1);
            REX_LOG_INFO("GfxDiag", "UpdateTexturePixels: tex={} {}x{} pitch={} hasTex={} state={}",
                         fmt::ptr(m_texture.Get()),
                         width, height, rowPitch,
                         m_hasTexture ? 1 : 0,
                         static_cast<int>(m_textureState));
        }
    }

    // First call or size change: (re)create texture + upload buffer
    if (!m_texture || m_texWidth != width || m_texHeight != height) {
        if (m_texture) {
            WaitForGPU();
            m_texture.Reset();
            m_textureUploadBuffer.Reset();
        }

        // Create DEFAULT texture (initial state: COPY_DEST)
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = static_cast<UINT64>(width);
        texDesc.Height           = static_cast<UINT>(height);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        m_textureState = D3D12_RESOURCE_STATE_COPY_DEST;
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            m_textureState, nullptr,
            IID_PPV_ARGS(&m_texture));
        if (FAILED(hr)) {
            REX_LOG_ERROR("D3D12", "Create texture for video failed: {:08x}", hr);
            if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "Failed to create video texture");
        }

        // Pre-allocate upload buffer (aligned to 256 for D3D12)
        UINT64 uploadSize = static_cast<UINT64>(rowPitch) * height;
        uploadSize = (uploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                     & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width            = uploadSize;
        uploadDesc.Height           = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels        = 1;
        uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
        uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.SampleDesc.Count = 1;

        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_textureUploadBuffer));
        if (FAILED(hr)) {
            REX_LOG_ERROR("D3D12", "Create upload buffer for video failed: {:08x}", hr);
            if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
            m_texture.Reset();
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "Failed to create video upload buffer");
        }

        // Create SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpu);
        m_textureSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        m_texWidth  = width;
        m_texHeight = height;
        m_hasTexture = true;
    }

    // Copy pixels to upload buffer
    void* mapped = nullptr;
    HRESULT hr = m_textureUploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Map failed");
    }

    {
        const uint8_t* src = pixels;
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < height; ++row) {
            memcpy(dst, src, rowPitch);
            src += rowPitch;
            dst += rowPitch;
        }
    }
    m_textureUploadBuffer->Unmap(0, nullptr);

    // Copy from upload buffer to texture
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyAllocator;
    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&copyAllocator));
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                     "CreateCommandAllocator failed");
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyList;
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     copyAllocator.Get(), nullptr,
                                     IID_PPV_ARGS(&copyList));
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                     "CreateCommandList failed");
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = m_texture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_textureUploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(width);
    srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(height);
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

    // [Stage 4] Log: resource state before copy
    {
        static auto s_nextLog = std::chrono::steady_clock::now();
        auto s_now = std::chrono::steady_clock::now();
        if (s_now >= s_nextLog) {
            s_nextLog = s_now + std::chrono::seconds(1);
            REX_LOG_INFO("GfxDiag", "CopyTextureRegion: tex={} beforeState={} {}x{} pitch={}",
                         fmt::ptr(m_texture.Get()),
                         static_cast<int>(m_textureState),
                         width, height, rowPitch);
        }
    }

    copyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Check if we need to transition from current state to COPY_DEST
    if (m_textureState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER toCopy = {};
        toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = m_texture.Get();
        toCopy.Transition.StateBefore = m_textureState;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyList->ResourceBarrier(1, &toCopy);
        REX_LOG_INFO("GfxDiag", "Barrier: state {} → COPY_DEST before copy",
                     static_cast<int>(m_textureState));
        m_textureState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    // Transition to shader-readable
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copyList->ResourceBarrier(1, &barrier);

    m_textureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    copyList->Close();

    ID3D12CommandList* lists[] = { copyList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    // Transition back to COPY_DEST for next frame update
    // (We leave it in PIXEL_SHADER_RESOURCE for rendering, then transition
    //  back to COPY_DEST at the start of the next UpdateTexturePixels call
    //  via the next frame's BeginFrame or the next update)

    return shared::Result<void>();
}

bool D3D12Renderer::UploadTextureToGPU(const TextureData& data) {
    // ── Create destination texture in DEFAULT heap ───────────────────────
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = static_cast<UINT64>(data.width);
    texDesc.Height           = static_cast<UINT>(data.height);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_texture));

    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommittedResource (texture) failed: {:08x}", hr);
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        return false;
    }

    // ── Create upload buffer ─────────────────────────────────────────────
    UINT64 uploadSize = static_cast<UINT64>(data.rowPitch) * data.height;
    // Ensure alignment to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
    uploadSize = (uploadSize + 511) & ~511ULL;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc.Count = 1;

    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommittedResource (upload) failed: {:08x}", hr);
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    // ── Copy pixel data to upload buffer ─────────────────────────────────
    void* mapped = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Map failed: {:08x}", hr);
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    // Row-by-row copy with stride matching the resource row pitch
    {
        const uint8_t* src = data.pixels.data();
        uint8_t* dst       = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < data.height; ++row) {
            std::memcpy(dst, src, static_cast<size_t>(data.rowPitch));
            src += data.rowPitch;
            dst += data.rowPitch;
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    // ── Create temporary command list for the copy ───────────────────────
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadAllocator;
    hr = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&uploadAllocator));

    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommandAllocator (upload) failed: {:08x}", hr);
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> uploadList;
    hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        uploadAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&uploadList));

    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "CreateCommandList (upload) failed: {:08x}", hr);
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    // ── CopyTextureRegion ────────────────────────────────────────────────
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource       = m_texture.Get();
    dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource                              = uploadBuffer.Get();
    srcLoc.Type                                   = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset                 = 0;
    srcLoc.PlacedFootprint.Footprint.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width        = static_cast<UINT>(data.width);
    srcLoc.PlacedFootprint.Footprint.Height       = static_cast<UINT>(data.height);
    srcLoc.PlacedFootprint.Footprint.Depth        = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch     = static_cast<UINT>(data.rowPitch);

    uploadList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // ── Transition texture to shader-readable state ──────────────────────
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    uploadList->ResourceBarrier(1, &barrier);

    uploadList->Close();

    // ── Execute and wait ─────────────────────────────────────────────────
    ID3D12CommandList* lists[] = { uploadList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    // ── Create SRV in the shader-visible descriptor heap ─────────────────
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpuHandle);
    m_textureSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

    m_hasTexture = true;
    m_texWidth  = data.width;
    m_texHeight = data.height;

    // uploadAllocator, uploadList, uploadBuffer released here (GPU is synced via WaitForGPU)
    return true;
}

// ─── Fill Mode Viewport Computation ─────────────────────────────────────────

void D3D12Renderer::ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const {
    float screenW = static_cast<float>(m_width);
    float screenH = static_cast<float>(m_height);

    // Default: full screen
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = screenW;
    vp.Height   = screenH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    scissor.left   = 0;
    scissor.top    = 0;
    scissor.right  = m_width;
    scissor.bottom = m_height;

    if (!m_hasTexture || m_texWidth <= 0 || m_texHeight <= 0) {
        return;
    }

    float texAspect = static_cast<float>(m_texWidth) / static_cast<float>(m_texHeight);
    float screenAspect = screenW / screenH;

    switch (m_fillMode) {
        case FillMode::Stretch:
            // Full screen, no adjustment needed
            break;

        case FillMode::Fit: {
            // Letterbox / pillarbox — viewport is the largest centered rect
            // that preserves aspect ratio
            float fitW, fitH;
            if (texAspect > screenAspect) {
                // Image is wider — pillarbox (black bars left/right)
                fitW = screenW;
                fitH = screenW / texAspect;
            } else {
                // Image is taller — letterbox (black bars top/bottom)
                fitH = screenH;
                fitW = screenH * texAspect;
            }
            float offsetX = (screenW - fitW) * 0.5f;
            float offsetY = (screenH - fitH) * 0.5f;
            vp.TopLeftX = offsetX;
            vp.TopLeftY = offsetY;
            vp.Width    = fitW;
            vp.Height   = fitH;
            break;
        }

        case FillMode::Fill: {
            // Scale to fill — image covers the screen, excess cropped
            // Viewport extends beyond screen, scissor clips to screen bounds
            float fillW, fillH;
            if (texAspect > screenAspect) {
                fillH = screenH;
                fillW = screenH * texAspect;
            } else {
                fillW = screenW;
                fillH = screenW / texAspect;
            }
            float offsetX = (screenW - fillW) * 0.5f;
            float offsetY = (screenH - fillH) * 0.5f;
            vp.TopLeftX = offsetX;
            vp.TopLeftY = offsetY;
            vp.Width    = fillW;
            vp.Height   = fillH;
            // Scissor stays at full screen — clips the oversized viewport
            break;
        }

        case FillMode::Center: {
            // Center at native resolution, clear color fills remaining area
            float cx = screenW * 0.5f;
            float cy = screenH * 0.5f;
            float hw = static_cast<float>(m_texWidth) * 0.5f;
            float hh = static_cast<float>(m_texHeight) * 0.5f;
            vp.TopLeftX = cx - hw;
            vp.TopLeftY = cy - hh;
            vp.Width    = static_cast<float>(m_texWidth);
            vp.Height   = static_cast<float>(m_texHeight);
            // Scissor clips to screen bounds to avoid drawing outside
            scissor.left   = std::max(0L, static_cast<LONG>(vp.TopLeftX));
            scissor.top    = std::max(0L, static_cast<LONG>(vp.TopLeftY));
            scissor.right  = std::min(static_cast<LONG>(m_width),  static_cast<LONG>(vp.TopLeftX + vp.Width));
            scissor.bottom = std::min(static_cast<LONG>(m_height), static_cast<LONG>(vp.TopLeftY + vp.Height));
            break;
        }

        case FillMode::Tile: {
            // Tile — the full quad maps multiple texture repeats via wrap
            // sampler. We render fullscreen, but the texture coords go
            // from 0..tileCount instead of 0..1.
            // This requires a vertex buffer update — handled separately.
            // For now, fall through to Stretch behavior.
            break;
        }
    }
}

} // namespace rex::wallpaper
