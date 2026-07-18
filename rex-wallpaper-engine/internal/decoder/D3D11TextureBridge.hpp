#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>

namespace rex::wallpaper {

// ─── SharedTexturePair ─────────────────────────────────────────────────────
// A D3D11 texture shared via NT handle, openable by a D3D12 device
// on the same physical GPU adapter.

struct SharedTexturePair {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11Texture;
    HANDLE sharedHandle = nullptr;
    int width  = 0;
    int height = 0;
};

// ─── D3D11TextureBridge ────────────────────────────────────────────────────
// Creates a D3D11 device on the same adapter as the given D3D12 device,
// and manages a pool of NV12 shared textures for D3D11↔D3D12 interop.

class D3D11TextureBridge {
public:
    D3D11TextureBridge();
    ~D3D11TextureBridge();

    /// Initialize the bridge. Finds the D3D11 adapter matching the D3D12
    /// device for shared handle compatibility.
    shared::Result<void> Initialize(ID3D12Device* d3d12Device, int width, int height);

    void Shutdown();

    /// Copy a source D3D11 NV12 texture into the shared texture and flush.
    /// Call when a new frame is decoded.
    shared::Result<void> CopyToShared(ID3D11Texture2D* sourceTexture);

    /// Get the shared handle for opening on the D3D12 side.
    HANDLE GetSharedHandle() const { return m_sharedPair.sharedHandle; }

    ID3D11Device*       GetDevice()      const { return m_d3d11Device.Get(); }
    ID3D11DeviceContext* GetContext()     const { return m_d3d11Context.Get(); }
    int  GetWidth()  const { return m_sharedPair.width; }
    int  GetHeight() const { return m_sharedPair.height; }
    bool IsValid()   const { return m_valid; }

private:
    shared::Result<void> FindMatchingAdapter(ID3D12Device* d3d12Device);
    shared::Result<void> CreateSharedTexture(int width, int height);

    Microsoft::WRL::ComPtr<ID3D11Device>        m_d3d11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  m_d3d11Context;
    SharedTexturePair m_sharedPair;
    bool m_valid = false;
};

// ─── D3D12 NV12 Texture Opener ─────────────────────────────────────────────
// Opens a shared D3D11 NV12 texture on the D3D12 device.
// Returns two SRVs: plane 0 (Y, R8_UNORM) and plane 1 (UV, R8G8_UNORM).

struct NV12D3D12Resources {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    D3D12_CPU_DESCRIPTOR_HANDLE srvY  = {};   // plane 0
    D3D12_CPU_DESCRIPTOR_HANDLE srvUV = {};   // plane 1
    D3D12_GPU_DESCRIPTOR_HANDLE srvYGpu  = {};
    D3D12_GPU_DESCRIPTOR_HANDLE srvUVGpu = {};
    int width  = 0;
    int height = 0;
};

shared::Result<NV12D3D12Resources> OpenSharedNV12Texture(
    ID3D12Device* device,
    HANDLE sharedHandle,
    int width,
    int height,
    ID3D12DescriptorHeap* srvHeap,
    UINT& srvOffset); // in/out: current offset in heap, updated after allocation

} // namespace rex::wallpaper
