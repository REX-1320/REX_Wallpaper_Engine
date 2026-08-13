#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>

namespace rex::graphics {

// ─── CommandQueue ───────────────────────────────────────────────────────────
//
// Wraps an ID3D12CommandQueue (DIRECT type) with a fence for GPU
// synchronization. Provides Execute(), WaitForGPU(), and Signal().
//
// Thread-safe: ExecuteCommandLists and Signal are free-threaded on
// the D3D12 command queue.

class CommandQueue {
public:
    CommandQueue() = default;
    ~CommandQueue();

    /// Create a DIRECT command queue on the given device.
    shared::Result<void> Initialize(ID3D12Device* device);

    void Shutdown();

    /// Execute one or more command lists.
    void Execute(ID3D12CommandList* const* lists, UINT count);

    /// Convenience: execute a single command list.
    void Execute(ID3D12CommandList* list);

    /// Block until the GPU has completed all submitted work.
    void WaitForGPU();

    /// Insert a GPU-side wait: the queue blocks until the given fence
    /// reaches `value` before executing subsequent work (orders D3D11
    /// video-engine writes before D3D12 sampling).
    void WaitForFence(ID3D12Fence* fence, UINT64 value);

    /// Signal the fence, returning the new fence value.
    UINT64 Signal();

    /// Get the underlying command queue.
    ID3D12CommandQueue* Get() const { return m_queue.Get(); }

    bool IsValid() const { return m_queue != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence>        m_fence;
    HANDLE                                    m_fenceEvent = nullptr;
    UINT64                                    m_fenceValue = 0;
};

} // namespace rex::graphics
