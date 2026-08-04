#pragma once
#include "IRenderer.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <string>
#include <atomic>

namespace rex::wallpaper {

class ShaderRenderer : public IRenderer {
public:
    ShaderRenderer();
    ~ShaderRenderer() override;

    bool load(const SceneInfo& scene, ::Compositor& compositor) override;
    void update(double deltaTime) override;
    void render(::Compositor& compositor) override;
    void unload() override;

    void play() override { m_playing.store(true); }
    void pause() override { m_playing.store(false); }
    void stop() override { m_playing.store(false); }
    bool isPlaying() const override { return m_playing.load(); }

private:
    bool InitD3D12();
    bool CompileShaders(const std::string& hlslSource);
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateSharedResources(int width, int height);
    void Draw();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    HANDLE m_fenceHandle = nullptr;
    UINT64 m_fenceVal = 0;
    UINT m_rtvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    ::Compositor* m_compositor = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::string m_path;
    std::atomic<bool> m_playing{false};
    double m_time = 0.0;

    static constexpr uint64_t LAYER_ID = 2;
};

} // namespace
