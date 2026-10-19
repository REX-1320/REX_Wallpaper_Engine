#include "RenderPipeline.hpp"
#include <shared/Logger.hpp>
#include <cstring>
#include <d3dcompiler.h>

namespace rex::wallpaper {

bool RenderPipeline::Initialize(ID3D12Device* device, int width, int height) {
    m_device = device;
    m_width = width;
    m_height = height;

    // Root constants: b0 = view-proj (16 floats), b1 = world (16 floats)
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 16;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sig, &err))) {
        REX_LOG_ERROR("Pipeline", "root sig serialize failed");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }

    // Shaders
    const char* vsSrc =
        "struct VSInput { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD; };"
        "struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };"
        "cbuffer VP : register(b0) { float4x4 viewProj; };"
        "cbuffer World : register(b1) { float4x4 world; };"
        "VSOutput main(VSInput input) {"
        "    VSOutput o;"
        "    o.pos = mul(mul(float4(input.pos, 1), world), viewProj);"
        "    o.uv = input.uv;"
        "    return o;"
        "}";

    const char* psSrc =
        "struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD; };"
        "float4 main(PSInput input) : SV_Target {"
        "    return float4(input.uv, 0.5 + 0.5*sin(input.uv.x*10), 1);"
        "}";

    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &vs, &err))) {
        REX_LOG_ERROR("Pipeline", "VS compile failed");
        return false;
    }
    if (FAILED(D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &ps, &err))) {
        REX_LOG_ERROR("Pipeline", "PS compile failed");
        return false;
    }

    // Input layout
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, 3 };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    if (FAILED(device->CreateGraphicsPipelineState(&pso,
        IID_PPV_ARGS(&m_pipelineState)))) {
        REX_LOG_ERROR("Pipeline", "PSO creation failed");
        return false;
    }

    // Depth buffer
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>(width);
    dd.Height = static_cast<UINT>(height);
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE dc = {};
    dc.Format = DXGI_FORMAT_D32_FLOAT;
    dc.DepthStencil.Depth = 1.0f;
    dc.DepthStencil.Stencil = 0;

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
        &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc,
        IID_PPV_ARGS(&m_depthTexture)))) {
        REX_LOG_ERROR("Pipeline", "depth buffer creation failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&dsvDesc,
        IID_PPV_ARGS(&m_dsvHeap)))) {
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depthTexture.Get(), &dsv,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    REX_LOG_INFO("Pipeline", "initialized ({}x{})", width, height);
    return true;
}

void RenderPipeline::Render(ID3D12GraphicsCommandList* cmdList,
                            const std::vector<Entity>& entities,
                            const XMMATRIX& viewProj,
                            const float clearColor[4]) {
    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pipelineState.Get());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sr = { 0, 0, m_width, m_height };
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // Bind per-frame VP
    XMMATRIX vpT = XMMatrixTranspose(viewProj);
    cmdList->SetGraphicsRoot32BitConstants(0, 16, &vpT, 0);

    for (const auto& e : entities) {
        if (!e.mesh) continue;
        if (e.mesh->vertexBufferId >= m_vertexBuffers.size()) continue;
        if (e.mesh->indexBufferId >= m_indexBuffers.size()) continue;

        XMMATRIX worldT = XMMatrixTranspose(e.transform.GetMatrix());
        cmdList->SetGraphicsRoot32BitConstants(1, 16, &worldT, 0);

        cmdList->IASetVertexBuffers(0, 1,
            &m_vertexBuffers[e.mesh->vertexBufferId].view);
        cmdList->IASetIndexBuffer(
            &m_indexBuffers[e.mesh->indexBufferId].view);
        cmdList->DrawIndexedInstanced(e.mesh->indexCount, 1, 0, 0, 0);
    }
}

uint32_t RenderPipeline::CreateVertexBuffer(const std::vector<SceneVertex>& vertices) {
    VBEntry entry;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(vertices.size() * sizeof(SceneVertex));
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
        &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) return UINT32_MAX;

    void* mapped;
    entry.resource->Map(0, nullptr, &mapped);
    memcpy(mapped, vertices.data(), vertices.size() * sizeof(SceneVertex));
    entry.resource->Unmap(0, nullptr);

    entry.view.BufferLocation = entry.resource->GetGPUVirtualAddress();
    entry.view.StrideInBytes = sizeof(SceneVertex);
    entry.view.SizeInBytes = static_cast<UINT>(vertices.size() * sizeof(SceneVertex));

    uint32_t id = (uint32_t)m_vertexBuffers.size();
    m_vertexBuffers.push_back(std::move(entry));
    return id;
}

uint32_t RenderPipeline::CreateIndexBuffer(const std::vector<uint32_t>& indices) {
    IBEntry entry;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(indices.size() * sizeof(uint32_t));
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
        &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) return UINT32_MAX;

    void* mapped;
    entry.resource->Map(0, nullptr, &mapped);
    memcpy(mapped, indices.data(), indices.size() * sizeof(uint32_t));
    entry.resource->Unmap(0, nullptr);

    entry.view.BufferLocation = entry.resource->GetGPUVirtualAddress();
    entry.view.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    entry.view.Format = DXGI_FORMAT_R32_UINT;

    uint32_t id = (uint32_t)m_indexBuffers.size();
    m_indexBuffers.push_back(std::move(entry));
    return id;
}

void RenderPipeline::Shutdown() {
    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_depthTexture.Reset();
    m_dsvHeap.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_device = nullptr;
}

} // namespace
