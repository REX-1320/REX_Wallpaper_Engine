#include "graphics/compositor/compositor.h"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include <cstdio>
#include <cstring>
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

    CreateSrvHeap();

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
}

bool Compositor::CreateDevice() {
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
    if (debugController) {
        debugController->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
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
    m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));

    m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator));

    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList));

    m_commandList->Close();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return true;
}

void Compositor::CreateSrvHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = MAX_TEXTURES;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap));
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

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
    factory->CreateSwapChainForHwnd(m_commandQueue.Get(), targetWindow,
        &swapChainDesc, nullptr, nullptr, &tempSwapChain);

    tempSwapChain.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    for (int i = 0; i < FRAME_COUNT; i++) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += i * m_rtvDescriptorSize;
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
    }

    return true;
}

void Compositor::PopulateCommandList() {
    // Transition back buffer from PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

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
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
}

void Compositor::RenderFrame() {
    if (!m_initialized) return;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());

    PopulateCommandList();
    m_commandList->Close();

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    HRESULT hr = m_swapChain->Present(1, 0);

    if (hr == DXGI_STATUS_OCCLUDED) {
        static int occludedCount = 0;
        occludedCount++;
        if (occludedCount % 300 == 1) {
            printf("[REX] Compositor: DXGI_STATUS_OCCLUDED "
                   "(count=%d). DWM considers the window occluded.\n",
                   occludedCount);
        }
    } else if (FAILED(hr)) {
        printf("[REX] Compositor: Present FAILED 0x%08lx\n",
               static_cast<unsigned long>(hr));
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
    if (!m_device) return false;

    int slot;
    if (!m_freeSrvSlots.empty()) {
        slot = m_freeSrvSlots.back();
        m_freeSrvSlots.pop_back();
    } else if (m_nextSrvSlot < MAX_TEXTURES) {
        slot = m_nextSrvSlot++;
    } else {
        printf("[REX] Compositor: out of SRV slots\n");
        return false;
    }

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        printf("[REX] Compositor: OpenSharedHandle failed (0x%08lx)\n",
               static_cast<unsigned long>(hr));
        m_freeSrvSlots.push_back(slot);
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    lt.srvSlot = slot;
    m_textures[layerId] = std::move(lt);

    printf("[REX] Compositor: imported texture for layer %llu (slot %d, %dx%d)\n",
        layerId, slot, width, height);
    return true;
}

void Compositor::RemoveLayerTexture(uint64_t layerId) {
    auto it = m_textures.find(layerId);
    if (it == m_textures.end()) return;

    m_freeSrvSlots.push_back(it->second.srvSlot);
    m_textures.erase(it);
    printf("[REX] Compositor: removed texture for layer %llu\n", layerId);
}

bool Compositor::UpdateLayerPixels(uint64_t layerId,
                                   const uint8_t* pixels,
                                   int width, int height, int rowPitch) {
    if (!m_device) return false;

    auto texIt = m_textures.find(layerId);
    if (texIt == m_textures.end()) {
        int slot;
        if (!m_freeSrvSlots.empty()) {
            slot = m_freeSrvSlots.back();
            m_freeSrvSlots.pop_back();
        } else if (m_nextSrvSlot < MAX_TEXTURES) {
            slot = m_nextSrvSlot++;
        } else {
            return false;
        }
        LayerTexture placeholder;
        placeholder.srvSlot = slot;
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
    }

    m_videoCopyList->Close();

    ID3D12CommandList* lists[] = { m_videoCopyList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    // Wait for copy to complete
    m_videoCopyFenceVal++;
    m_commandQueue->Signal(m_videoCopyFence.Get(), m_videoCopyFenceVal);
    if (m_videoCopyFence->GetCompletedValue() < m_videoCopyFenceVal) {
        m_videoCopyFence->SetEventOnCompletion(m_videoCopyFenceVal,
                                                m_videoCopyFenceEvent);
        WaitForSingleObject(m_videoCopyFenceEvent, INFINITE);
    }

    return true;
}

static void LogToFile(const char* msg) {
    FILE* f = fopen("C:\\Users\\munna\\AppData\\Local\\Temp\\rex_bg_log.txt", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
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

    int slot;
    if (!m_freeSrvSlots.empty()) {
        slot = m_freeSrvSlots.back();
        m_freeSrvSlots.pop_back();
    } else if (m_nextSrvSlot < MAX_TEXTURES) {
        slot = m_nextSrvSlot++;
    } else {
        LogToFile("  FAIL: out of SRV slots");
        return false;
    }
    snprintf(buf, sizeof(buf), "  Allocated SRV slot %d", slot);
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
        m_freeSrvSlots.push_back(slot); return false;
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
        m_freeSrvSlots.push_back(slot); return false;
    }
    LogToFile("  Created upload buffer OK");

    void* mapped = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) { LogToFile("  FAIL: Map upload buffer"); m_freeSrvSlots.push_back(slot); return false; }
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
    if (FAILED(hr)) { LogToFile("  FAIL: create copy allocator"); m_freeSrvSlots.push_back(slot); return false; }

    ComPtr<ID3D12GraphicsCommandList> copyList;
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      copyAlloc.Get(), nullptr,
                                      IID_PPV_ARGS(&copyList));
    if (FAILED(hr)) { LogToFile("  FAIL: create copy list"); m_freeSrvSlots.push_back(slot); return false; }
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
    if (FAILED(hr)) { LogToFile("  FAIL: create fence"); m_freeSrvSlots.push_back(slot); return false; }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) { LogToFile("  FAIL: create event"); m_freeSrvSlots.push_back(slot); return false; }

    UINT64 fenceVal = 1;
    m_commandQueue->Signal(fence.Get(), fenceVal);
    if (fence->GetCompletedValue() < fenceVal) {
        fence->SetEventOnCompletion(fenceVal, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
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
