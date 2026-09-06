#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>
#include <mutex>
#include <optional>

namespace rex::wallpaper {

// ─── DescriptorAllocation ──────────────────────────────────────────────────

struct DescriptorAllocation {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    UINT index = UINT_MAX;   // 0-based index within the heap
};

// ─── DescriptorHeapManager ─────────────────────────────────────────────────
//
// Manages a D3D12 shader-visible descriptor heap (CBV_SRV_UAV type).
// Allocates descriptors via a free-list for efficient reuse.
//
// Usage:
//   1. Create with desired capacity (e.g., 256 descriptors)
//   2. Allocate() returns a DescriptorAllocation
//   3. Free(allocation) returns the slot to the free list
//
// Thread-safe for allocation/free from any thread.

class DescriptorHeapManager {
public:
    DescriptorHeapManager() = default;
    ~DescriptorHeapManager() = default;

    /// Initialize the heap. Must be called once with a valid D3D12 device.
    bool Initialize(ID3D12Device* device, UINT capacity = 256);

    /// Allocate a single descriptor. Returns nullopt when full.
    std::optional<DescriptorAllocation> Allocate();

    /// Return a descriptor slot to the free list.
    void Free(DescriptorAllocation alloc);

    /// Direct access to the heap for binding.
    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }

    UINT GetCapacity() const { return m_capacity; }
    UINT GetAllocatedCount() const;
    UINT GetDescriptorSize() const { return m_descriptorSize; }

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_capacity = 0;
    UINT m_descriptorSize = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};

    std::vector<UINT> m_freeList;       // stack of free indices
    std::vector<bool> m_allocated;      // true if index is currently allocated
    mutable std::mutex m_mutex;
};

} // namespace rex::wallpaper
