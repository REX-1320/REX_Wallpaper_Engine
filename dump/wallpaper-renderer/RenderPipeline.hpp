#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>
#include "ECS.hpp"

namespace rex::wallpaper {

using namespace DirectX;

struct SceneVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

class RenderPipeline {
public:
    bool Initialize(ID3D12Device* device, int width, int height);
    void Render(ID3D12GraphicsCommandList* cmdList,
                const std::vector<Entity>& entities,
                const XMMATRIX& viewProj,
                const float clearColor[4]);
    void Shutdown();

    uint32_t CreateVertexBuffer(const std::vector<SceneVertex>& vertices);
    uint32_t CreateIndexBuffer(const std::vector<uint32_t>& indices);
    uint32_t GetVertexBufferCount() const { return (uint32_t)m_vertexBuffers.size(); }

    ID3D12Resource* GetDepthBuffer() const { return m_depthTexture.Get(); }
    ID3D12DescriptorHeap* GetDsvHeap() const { return m_dsvHeap.Get(); }

private:
    struct VBEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_VERTEX_BUFFER_VIEW view = {};
    };
    struct IBEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_INDEX_BUFFER_VIEW view = {};
    };

    ID3D12Device* m_device = nullptr;
    int m_width = 0, m_height = 0;

    std::vector<VBEntry> m_vertexBuffers;
    std::vector<IBEntry> m_indexBuffers;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
};

} // namespace
