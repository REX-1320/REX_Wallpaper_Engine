#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>
#include <string>

namespace rex::graphics {

// ─── Device ─────────────────────────────────────────────────────────────────
//
// Wraps ID3D12Device creation and provides factory methods for common D3D12
// objects (command allocators, command lists, descriptor heaps, root
// signatures, pipeline states, committed resources).
//
// Thread-safe: the underlying device is free-threaded.

class Device {
public:
    Device() = default;
    ~Device();

    /// Create the D3D12 device with debug layer in Debug builds.
    /// @param minimumFeatureLevel Minimum feature level (default 11_0).
    shared::Result<void> Initialize(
        D3D_FEATURE_LEVEL minimumFeatureLevel = D3D_FEATURE_LEVEL_11_0);

    /// Adopt an existing D3D12 device (shared with other components).
    /// Guarantees every renderer uses the SAME device — required for
    /// cross-device shared handles (video pool textures/fence) to stay
    /// valid, regardless of adapter topology.
    shared::Result<void> InitializeAdopted(ID3D12Device* device);

    /// Returns the maximum supported feature level.
    D3D_FEATURE_LEVEL MaxFeatureLevel() const { return m_maxFeatureLevel; }

    /// Returns the DXGI adapter used by this device.
    Microsoft::WRL::ComPtr<IDXGIAdapter4> Adapter() const { return m_adapter; }

    /// Get the underlying D3D12 device.
    ID3D12Device* Get() const { return m_device.Get(); }

    // ─── Factory helpers ──────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) const;

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CreateCommandList(
        ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initialPso = nullptr) const;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT numDescriptors,
        D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE) const;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(
        const void* serializedBlob, SIZE_T blobSize) const;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const;

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES& heapProps,
        D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initial,
        const D3D12_CLEAR_VALUE* clearValue = nullptr) const;

    Microsoft::WRL::ComPtr<IDXGIFactory6> CreateFactory() const;

    UINT GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const {
        return m_device->GetDescriptorHandleIncrementSize(type);
    }

    // ─── Device removal ──────────────────────────────────────────────────
    HRESULT GetDeviceRemovedReason() const {
        return m_device ? m_device->GetDeviceRemovedReason() : S_OK;
    }

    bool IsValid() const { return m_device != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12Device>    m_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter4>   m_adapter;
    D3D_FEATURE_LEVEL                       m_maxFeatureLevel = D3D_FEATURE_LEVEL_11_0;
};

} // namespace rex::graphics
