#include "Device.hpp"
#include <shared/Logger.hpp>

namespace rex::graphics {

Device::~Device() = default;

shared::Result<void> Device::Initialize(D3D_FEATURE_LEVEL minimumFeatureLevel) {
    if (m_device) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Device already initialized");
    }

#ifdef _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            REX_LOG_INFO("Device", "D3D12 debug layer enabled");
        }
    }
#endif

    // ── Create DXGI factory and enumerate adapters ──────────────────────
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateDXGIFactory2 failed");
    }

    // Prefer hardware adapter
    Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC3 desc;
        if (SUCCEEDED(adapter->GetDesc3(&desc))) {
            // Skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            // Check if this adapter supports D3D12 at the requested level
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), minimumFeatureLevel,
                                            __uuidof(ID3D12Device), nullptr))) {
                m_adapter = adapter;
                REX_LOG_INFO("Device", "Selected adapter: {} (D3D_FEATURE_LEVEL_{}.{})",
                             reinterpret_cast<const char*>(desc.Description),
                             (minimumFeatureLevel >> 12) & 0xF,
                             (minimumFeatureLevel >> 8) & 0xF);
                break;
            }
        }
        adapter.Reset();
    }

    // Fall back to default adapter if no hardware adapter matched
    if (!m_adapter) {
        hr = D3D12CreateDevice(nullptr, minimumFeatureLevel, IID_PPV_ARGS(&m_device));
    } else {
        hr = D3D12CreateDevice(m_adapter.Get(), minimumFeatureLevel, IID_PPV_ARGS(&m_device));
    }

    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "D3D12CreateDevice failed");
    }

    // ── Query feature level ─────────────────────────────────────────────
    D3D12_FEATURE_DATA_FEATURE_LEVELS featLevels = {};
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    featLevels.NumFeatureLevels = ARRAYSIZE(levels);
    featLevels.pFeatureLevelsRequested = levels;

    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS, &featLevels, sizeof(featLevels)))) {
        m_maxFeatureLevel = featLevels.MaxSupportedFeatureLevel;
    }

    REX_LOG_INFO("Device", "D3D12 device created (max feature level: {}.{})",
                 (m_maxFeatureLevel >> 12) & 0xF,
                 (m_maxFeatureLevel >> 8) & 0xF);

    m_device->SetName(L"REX D3D12 Device");
    return shared::Result<void>();
}

shared::Result<void> Device::InitializeAdopted(ID3D12Device* device) {
    if (m_device) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Device already initialized");
    }
    if (!device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Adopted device is null");
    }

    m_device = device;

    // Query the adapter and max feature level for telemetry parity with
    // self-created devices.
    Microsoft::WRL::ComPtr<IDXGIDevice4> dxgiDevice;
    if (SUCCEEDED(m_device.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            adapter.As(&m_adapter);
        }
    }

    D3D12_FEATURE_DATA_FEATURE_LEVELS featLevels = {};
    // SDK 26100+: feature levels use the shared D3D_FEATURE_LEVEL enum
    // (D3D12_FEATURE_LEVEL no longer exists).
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    featLevels.NumFeatureLevels = ARRAYSIZE(levels);
    featLevels.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS, &featLevels, sizeof(featLevels)))) {
        m_maxFeatureLevel = featLevels.MaxSupportedFeatureLevel;
    }

    REX_LOG_INFO("Device", "Adopted D3D12 device (max feature level: {}.{})",
                 (m_maxFeatureLevel >> 12) & 0xF,
                 (m_maxFeatureLevel >> 8) & 0xF);
    return shared::Result<void>();
}

// ─── Factory helpers ───────────────────────────────────────────────────────

Microsoft::WRL::ComPtr<ID3D12CommandAllocator> Device::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type) const {

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateCommandAllocator failed: {:08x}", hr);
    }
    return allocator;
}

Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> Device::CreateCommandList(
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialPso) const {

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    HRESULT hr = m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, initialPso,
        IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateCommandList failed: {:08x}", hr);
    }
    return list;
}

Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Device::CreateDescriptorHeap(
    D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors,
    D3D12_DESCRIPTOR_HEAP_FLAGS flags) const {

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;
    desc.Flags = flags;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateDescriptorHeap failed: {:08x}", hr);
    }
    return heap;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> Device::CreateRootSignature(
    const void* serializedBlob, SIZE_T blobSize) const {

    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    HRESULT hr = m_device->CreateRootSignature(
        0, serializedBlob, blobSize, IID_PPV_ARGS(&signature));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateRootSignature failed: {:08x}", hr);
    }
    return signature;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> Device::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const {

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateGraphicsPipelineState failed: {:08x}", hr);
    }
    return pso;
}

Microsoft::WRL::ComPtr<ID3D12Resource> Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES& heapProps, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial,
    const D3D12_CLEAR_VALUE* clearValue) const {

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, flags, &desc, initial, clearValue, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateCommittedResource failed: {:08x}", hr);
    }
    return resource;
}

Microsoft::WRL::ComPtr<IDXGIFactory6> Device::CreateFactory() const {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateDXGIFactory2 failed: {:08x}", hr);
    }
    return factory;
}

} // namespace rex::graphics
