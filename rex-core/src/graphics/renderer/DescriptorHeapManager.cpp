#include "DescriptorHeapManager.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper {

bool DescriptorHeapManager::Initialize(ID3D12Device* device, UINT capacity) {
    if (!device || capacity == 0) return false;

    m_capacity = capacity;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = capacity;
    desc.Type            = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags           = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        REX_LOG_ERROR("HeapMgr", "CreateDescriptorHeap failed: {:08x}", hr);
        return false;
    }

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

    // Pre-fill free list with all indices (in reverse order so we pop from back)
    m_freeList.reserve(capacity);
    for (UINT i = 0; i < capacity; ++i) {
        m_freeList.push_back(capacity - 1 - i); // stack: 255, 254, ..., 0
    }
    m_allocated.assign(capacity, false);

    REX_LOG_INFO("HeapMgr", "Initialized descriptor heap with {} slots ({} bytes each)",
                 capacity, m_descriptorSize);
    return true;
}

std::optional<DescriptorAllocation> DescriptorHeapManager::Allocate() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_freeList.empty()) {
        REX_LOG_ERROR("HeapMgr", "Descriptor heap exhausted ({} slots)", m_capacity);
        return std::nullopt;
    }

    UINT index = m_freeList.back();
    m_freeList.pop_back();
    m_allocated[index] = true;

    DescriptorAllocation alloc;
    alloc.index     = index;
    alloc.cpuHandle = { m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize };
    alloc.gpuHandle = { m_gpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize };

    return alloc;
}

void DescriptorHeapManager::Free(DescriptorAllocation alloc) {
    if (alloc.index >= m_capacity) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_allocated[alloc.index]) {
        REX_LOG_WARN("HeapMgr", "Attempt to free unallocated descriptor index {}", alloc.index);
        return;
    }
    m_allocated[alloc.index] = false;
    m_freeList.push_back(alloc.index);
}

UINT DescriptorHeapManager::GetAllocatedCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capacity - static_cast<UINT>(m_freeList.size());
}

} // namespace rex::wallpaper
