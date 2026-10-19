#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <vector>
#include <unordered_map>
#include <cstdint>

using Microsoft::WRL::ComPtr;

struct Layer {
    uint64_t id;
    uint32_t kind;
    ComPtr<ID3D12Resource> texture;
    float bounds[4];
    float opacity;
    int zOrder;
    bool visible;
};

// Per-back-buffer D3D12 states (audit 5.8 #9). A back buffer must not be
// assumed to be in PRESENT: a frame whose Present failed, or a buffer whose
// frame never finished, stays in whatever state its last recorded barrier
// set — and the next transition must start from THAT state.
class BackBufferStateTracker {
public:
    static constexpr int COUNT = 3;

    D3D12_RESOURCE_STATES Before(int index) const { return m_state[index]; }
    void OnRenderBegin(int index) { m_state[index] = D3D12_RESOURCE_STATE_RENDER_TARGET; }
    void OnRenderEnd(int index) { m_state[index] = D3D12_RESOURCE_STATE_PRESENT; }
    void Reset() {
        for (auto& s : m_state) s = D3D12_RESOURCE_STATE_PRESENT;
    }

private:
    // Flip-model swap-chain buffers begin life in the PRESENT state.
    D3D12_RESOURCE_STATES m_state[COUNT] = {
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_PRESENT,
    };
};

class Compositor {
public:
    Compositor();
    ~Compositor();

    bool Initialize(HWND targetWindow);
    void Shutdown();
    void RenderFrame();

    void SubmitLayer(const Layer& layer);
    void RemoveLayer(uint64_t id);
    void SetClearColor(float r, float g, float b);

    // Imports a shared texture as a layer's SRV. Format and dimensions are
    // derived from the resource desc (audit 5.8 #7), not from the caller's
    // numbers. Synchronisation contract (audit 5.8 #8): the compositor only
    // samples the texture and records no acquire barrier — the producer must
    // have finished writing before this call (e.g. via its own shared-fence
    // wait) and must not write the resource while the compositor samples it.
    bool ImportLayerTexture(uint64_t layerId, HANDLE sharedHandle, int width, int height);
    void RemoveLayerTexture(uint64_t layerId);

    bool SetBackgroundFromFile(const char* filePath);
    bool UpdateLayerPixels(uint64_t layerId, const uint8_t* pixels,
                           int width, int height, int rowPitch);

    float GetViewportWidth()  const { return m_viewportWidth; }
    float GetViewportHeight() const { return m_viewportHeight; }

    static constexpr uint32_t MAX_TEXTURES = 64;
    // Bounded GPU waits (audit 5.8 #5): an INFINITE wait on a hung GPU froze
    // the compositor with no escape and no device-removed check.
    static constexpr unsigned long GPU_WAIT_TIMEOUT_MS = 2000;

    // ── Failure-path classification (audit 5.8 #3/#5/#9) ────────────────────
    // Pure helpers so the device-lost / timeout / present outcomes are
    // unit-testable without a GPU; production calls them verbatim.
    enum class PresentOutcome { Ok, Occluded, DeviceLost, Failed };
    static PresentOutcome ClassifyPresent(HRESULT hr);

    enum class GpuWaitOutcome { Completed, TimedOut, DeviceLost, WaitFailed };
    static GpuWaitOutcome ClassifyGpuWait(unsigned long waitResult,
                                          HRESULT deviceRemovedReason);

    static bool IsDeviceLostHr(HRESULT hr);

    // Shared-handle imports derive the SRV format from the resource itself
    // (audit 5.8 #7); TYPELESS resources get their canonical typed view.
    static DXGI_FORMAT SrvFormatForResourceFormat(DXGI_FORMAT format);

    // SRV slot bookkeeping (audit 5.8 #6): replacing a layer recycles its
    // slot instead of leaking it until the 64-slot heap is exhausted.
    static bool TakeSrvSlot(std::vector<uint32_t>& freeSlots, uint32_t& nextSlot,
                            uint32_t maxSlots, uint32_t& outSlot);
    static void ReturnSrvSlot(std::vector<uint32_t>& freeSlots, int slot);

private:
    bool CreateDevice();
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateSwapChain(HWND targetWindow);
    bool CreateSrvHeap();
    bool CreateVertexBuffer();
    void PopulateCommandList();
    // Bounded fence wait + device check (audit 5.8 #5). Returns false on
    // timeout/failure (and latches device-lost when the reason says so).
    bool WaitForGpuFence(ID3D12Fence* fence, UINT64 value, HANDLE eventHandle,
                         const char* what);
    void MarkDeviceLost(const char* what);
    // Returns an existing layer texture's SRV slot to the free list so a
    // replacement reuses it (audit 5.8 #6).
    void RecycleTextureSlot(uint64_t layerId);

    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    static constexpr int FRAME_COUNT = 3;
    ComPtr<ID3D12Resource> m_renderTargets[FRAME_COUNT];
    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;
    UINT m_frameIndex = 0;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};
    uint32_t m_indexCount = 0;

    struct LayerTexture {
        ComPtr<ID3D12Resource> resource;
        int srvSlot = -1;
        int texWidth = 0;
        int texHeight = 0;
        // D3D12 state the resource is currently in (audit 5.8 #4). Barriers
        // must name this as StateBefore — the texture is COPY_DEST only
        // right after creation and PIXEL_SHADER_RESOURCE afterwards.
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    };
    std::unordered_map<uint64_t, LayerTexture> m_textures;
    std::vector<uint32_t> m_freeSrvSlots;
    uint32_t m_nextSrvSlot = 0;

    std::vector<Layer> m_layers;
    float m_clearColor[4] = { 0.0f, 0.5f, 0.8f, 1.0f };
    float m_viewportWidth = 800.0f;
    float m_viewportHeight = 600.0f;

    // Main-queue frame fence (audit 5.8 #3): RenderFrame waits it before
    // resetting the allocator/list, Shutdown drains it before releasing.
    ComPtr<ID3D12Fence> m_frameFence;
    HANDLE m_frameFenceEvent = nullptr;
    UINT64 m_frameFenceVal = 0;
    // Latched once the device is gone (device-removed Present/wait/failure):
    // rendering stops instead of repeatedly touching a dead device.
    bool m_deviceLost = false;
    BackBufferStateTracker m_backBufferStates;

    // Reusable video frame upload resources
    ComPtr<ID3D12Resource> m_videoUploadBuffer;
    ComPtr<ID3D12CommandAllocator> m_videoCopyAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_videoCopyList;
    ComPtr<ID3D12Fence> m_videoCopyFence;
    HANDLE m_videoCopyFenceEvent = nullptr;
    UINT64 m_videoCopyFenceVal = 0;
    uint32_t m_videoUploadSize = 0;

    bool m_initialized = false;
};
