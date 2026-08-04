#include "ShaderRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <fstream>
#include <sstream>
#include <d3dcompiler.h>


namespace rex::wallpaper {

struct ShaderCB {
    float time, sinTime, cosTime, pad0;
    float width, height, invWidth, invHeight;
};

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

ShaderRenderer::ShaderRenderer() = default;
ShaderRenderer::~ShaderRenderer() { unload(); }

bool ShaderRenderer::load(const SceneInfo& scene, ::Compositor& compositor) {
    m_path = scene.sourcePath;
    if (m_path.empty()) return false;

    std::string hlsl;
    std::ifstream file(m_path);
    if (file.good()) {
        std::stringstream buf;
        buf << file.rdbuf();
        hlsl = buf.str();
    } else {
        hlsl = m_path;
    }
    if (hlsl.empty()) return false;

    m_compositor = &compositor;
    m_width = static_cast<int>(compositor.GetViewportWidth());
    m_height = static_cast<int>(compositor.GetViewportHeight());

    if (!InitD3D12()) { Log("[ShaderRenderer] D3D12 init failed\n"); return false; }
    if (!CompileShaders(hlsl)) { Log("[ShaderRenderer] shader compile failed\n"); return false; }
    if (!CreateRootSignature()) { Log("[ShaderRenderer] root sig failed\n"); return false; }
    if (!CreatePipelineState()) { Log("[ShaderRenderer] PSO failed\n"); return false; }
    if (!CreateSharedResources(m_width, m_height)) { Log("[ShaderRenderer] shared resources failed\n"); return false; }

    compositor.SubmitLayer({ LAYER_ID, 0, nullptr,
        { 0, 0, static_cast<float>(m_width), static_cast<float>(m_height) },
        1.0f, 0, true });

    m_playing.store(true);
    Log("[ShaderRenderer] loaded (%dx%d)\n", m_width, m_height);
    return true;
}

void ShaderRenderer::update(double deltaTime) {
    m_time += deltaTime;
}

void ShaderRenderer::render(::Compositor& compositor) {
    if (!m_playing.load() || !m_device) return;

    // update constant buffer
    {
        ShaderCB data;
        data.time = static_cast<float>(m_time);
        data.sinTime = sinf(data.time);
        data.cosTime = cosf(data.time);
        data.pad0 = 0.0f;
        data.width = static_cast<float>(m_width);
        data.height = static_cast<float>(m_height);
        data.invWidth = 1.0f / data.width;
        data.invHeight = 1.0f / data.height;

        void* mapped;
        m_constantBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, &data, sizeof(data));
        m_constantBuffer->Unmap(0, nullptr);
    }

    Draw();

    // signal fence for compositor
    m_fenceVal++;
    m_cmdQueue->Signal(m_sharedFence.Get(), m_fenceVal);
    compositor.WaitForExternalFence(m_fenceHandle, m_fenceVal);
}

void ShaderRenderer::unload() {
    m_playing.store(false);

    // wait for GPU
    if (m_cmdQueue && m_sharedFence) {
        m_fenceVal++;
        m_cmdQueue->Signal(m_sharedFence.Get(), m_fenceVal);

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (event) {
            m_sharedFence->SetEventOnCompletion(m_fenceVal, event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
    }

    if (m_compositor) {
        m_compositor->RemoveLayerTexture(LAYER_ID);
        m_compositor->RemoveLayer(LAYER_ID);
        m_compositor = nullptr;
    }

    m_sharedTexture.Reset();
    m_constantBuffer.Reset();
    m_sharedFence.Reset();
    if (m_fenceHandle) { CloseHandle(m_fenceHandle); m_fenceHandle = nullptr; }
    m_rtvHeap.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_cmdList.Reset();
    m_cmdAllocator.Reset();
    m_cmdQueue.Reset();
    m_device.Reset();
}

void ShaderRenderer::Draw() {
    m_cmdAllocator->Reset();
    m_cmdList->Reset(m_cmdAllocator.Get(), m_pipelineState.Get());

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float clear[4] = { 0, 0, 0, 1 };
    m_cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

    D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1 };
    D3D12_RECT sr = { 0, 0, m_width, m_height };
    m_cmdList->RSSetViewports(1, &vp);
    m_cmdList->RSSetScissorRects(1, &sr);

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->DrawInstanced(3, 1, 0, 0);

    m_cmdList->Close();

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
}

bool ShaderRenderer::InitD3D12() {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)))) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)))) return false;

    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_cmdAllocator)))) return false;

    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)))) return false;
    m_cmdList->Close();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return true;
}

bool ShaderRenderer::CompileShaders(const std::string& hlslSource) {
    const char* vsSrc =
        "struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };"
        "VSOutput mainVS(uint id : SV_VertexID) {"
        "    float2 pos[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };"
        "    VSOutput o;"
        "    o.pos = float4(pos[id], 0, 1);"
        "    o.uv = float2(o.pos.x * 0.5 + 0.5, 1.0 - (o.pos.y * 0.5 + 0.5));"
        "    return o;"
        "}";

    ComPtr<ID3DBlob> err;
    if (FAILED(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
        "mainVS", "vs_5_0", 0, 0, &m_vsBlob, &err))) {
        Log("[ShaderRenderer] VS compile failed: %s\n",
            err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    if (FAILED(D3DCompile(hlslSource.c_str(), hlslSource.size(), nullptr, nullptr, nullptr,
        "mainPS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &m_psBlob, &err))) {
        Log("[ShaderRenderer] PS compile failed: %s\n",
            err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    return true;
}

bool ShaderRenderer::CreateRootSignature() {
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sig, &err))) {
        Log("[ShaderRenderer] serialize root sig failed\n");
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }
    return true;
}

bool ShaderRenderer::CreatePipelineState() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = { m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize() };
    pso.PS = { m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize() };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateGraphicsPipelineState(&pso,
        IID_PPV_ARGS(&m_pipelineState)))) {
        return false;
    }
    return true;
}

bool ShaderRenderer::CreateSharedResources(int width, int height) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>(width);
    td.Height = static_cast<UINT>(height);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 0; cv.Color[1] = 0; cv.Color[2] = 0; cv.Color[3] = 1;

    if (FAILED(m_device->CreateCommittedResource(&hp,
        D3D12_HEAP_FLAG_SHARED, &td,
        D3D12_RESOURCE_STATE_COMMON, &cv,
        IID_PPV_ARGS(&m_sharedTexture)))) {
        return false;
    }

    HANDLE sharedHandle = nullptr;
    if (FAILED(m_device->CreateSharedHandle(m_sharedTexture.Get(), nullptr,
        GENERIC_ALL, L"REXShaderFrame", &sharedHandle))) {
        return false;
    }

    m_compositor->ImportLayerTexture(LAYER_ID, sharedHandle, width, height);
    CloseHandle(sharedHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(m_sharedTexture.Get(), nullptr, rtv);

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&m_sharedFence)))) {
        return false;
    }
    if (FAILED(m_device->CreateSharedHandle(m_sharedFence.Get(), nullptr,
        GENERIC_ALL, L"REXShaderFence", &m_fenceHandle))) {
        return false;
    }
    m_fenceVal = 0;

    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateCommittedResource(&hp,
        D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_constantBuffer)))) {
        return false;
    }

    return true;
}

} // namespace
