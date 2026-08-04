#pragma once
#include "IRenderer.hpp"
#include "ECS.hpp"
#include "RenderPipeline.hpp"
#include "SceneLoader.hpp"
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include <atomic>

class Compositor;

namespace rex::wallpaper {

class SceneRenderer : public IRenderer {
public:
    SceneRenderer();
    ~SceneRenderer() override;

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
    void UpdateCamera(double dt);
    void DrawFrame();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTexture;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    HANDLE m_fenceHandle = nullptr;
    UINT64 m_fenceValue = 1;
    UINT m_rtvDescriptorSize = 0;

    ECS m_ecs;
    RenderPipeline m_pipeline;
    SceneLoader m_loader;
    float m_cameraAngle = 0.0f;
    float m_orbitRadius = 5.0f;
    double m_time = 0.0;

    int m_width = 0, m_height = 0;
    std::atomic<bool> m_playing{false};
    std::string m_path;
    Compositor* m_compositor = nullptr;
    static constexpr uint64_t SCENE_LAYER_ID = 3;
};

} // namespace
