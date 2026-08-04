#include "SceneRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>

namespace rex::wallpaper {

SceneRenderer::SceneRenderer() = default;
SceneRenderer::~SceneRenderer() { unload(); }

bool SceneRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_path = scene.sourcePath;
    m_width  = static_cast<int>(compositor.GetViewportWidth());
    m_height = static_cast<int>(compositor.GetViewportHeight());

    printf("[SceneRenderer] loading scene: %s (%dx%d)\n",
           m_path.c_str(), m_width, m_height);

    m_compositor = &compositor;
    if (!InitD3D12(compositor, m_width, m_height)) return false;

    if (!m_loader.Load(m_path, m_ecs, m_pipeline)) {
        printf("[SceneRenderer] failed to load scene\n");
        return false;
    }

    compositor.SubmitLayer({ SCENE_LAYER_ID, 0, nullptr,
        { 0, 0, (float)m_width, (float)m_height },
        1.0f, 0, true });

    m_playing.store(true);
    printf("[SceneRenderer] loaded (%zu entities)\n", m_ecs.GetEntities().size());
    return true;
}

void SceneRenderer::update(double deltaTime) {
    m_time += deltaTime;
    UpdateCamera(deltaTime);
}

void SceneRenderer::render(Compositor& compositor) {
    if (!m_playing.load() || !m_device) return;

    DrawFrame();

    m_fenceValue++;
    m_commandQueue->Signal(m_sharedFence.Get(), m_fenceValue);
    compositor.WaitForExternalFence(m_fenceHandle, m_fenceValue);
}

void SceneRenderer::unload() {
    m_playing.store(false);

    if (m_commandQueue && m_sharedFence) {
        m_fenceValue++;
        m_commandQueue->Signal(m_sharedFence.Get(), m_fenceValue);
        HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            m_sharedFence->SetEventOnCompletion(m_fenceValue, ev);
            WaitForSingleObject(ev, INFINITE);
            CloseHandle(ev);
        }
    }

    if (m_compositor) {
        m_compositor->RemoveLayerTexture(SCENE_LAYER_ID);
        m_compositor->RemoveLayer(SCENE_LAYER_ID);
        m_compositor = nullptr;
    }

    m_pipeline.Shutdown();

    m_sharedTexture.Reset();
    m_rtvHeap.Reset();
    m_sharedFence.Reset();
    if (m_fenceHandle) { CloseHandle(m_fenceHandle); m_fenceHandle = nullptr; }
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
}

bool SceneRenderer::InitD3D12(Compositor& compositor, int width, int height) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&m_device)))) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_commandQueue))))
        return false;

    // Shared texture
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)width;
    td.Height = (UINT)height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 0; cv.Color[1] = 0; cv.Color[2] = 0; cv.Color[3] = 1;

    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED,
        &td, D3D12_RESOURCE_STATE_COMMON, &cv,
        IID_PPV_ARGS(&m_sharedTexture)))) {
        return false;
    }

    HANDLE sharedHandle = nullptr;
    if (FAILED(m_device->CreateSharedHandle(m_sharedTexture.Get(), nullptr,
        GENERIC_ALL, L"REXSceneFrame", &sharedHandle))) {
        return false;
    }
    compositor.ImportLayerTexture(SCENE_LAYER_ID, sharedHandle, width, height);
    CloseHandle(sharedHandle);

    // Shared fence
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&m_sharedFence)))) {
        return false;
    }
    if (FAILED(m_device->CreateSharedHandle(m_sharedFence.Get(), nullptr,
        GENERIC_ALL, L"REXSceneFence", &m_fenceHandle))) {
        return false;
    }
    m_fenceValue = 0;

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc,
        IID_PPV_ARGS(&m_rtvHeap)))) {
        return false;
    }
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(m_sharedTexture.Get(), &rtv,
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // Command list
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)))) return false;
    m_commandList->Close();

    // Initialize pipeline (creates the depth buffer too)
    if (!m_pipeline.Initialize(m_device.Get(), width, height)) return false;

    printf("[SceneRenderer] D3D12 initialized\n");
    return true;
}

void SceneRenderer::UpdateCamera(double dt) {
    m_cameraAngle += (float)dt * 0.3f;
    // Find active camera entity and orbit it
    for (auto& e : m_ecs.GetEntities()) {
        if (e.camera && e.camera->active) {
            float x = sinf(m_cameraAngle) * m_orbitRadius;
            float z = cosf(m_cameraAngle) * m_orbitRadius;
            e.transform.position = XMFLOAT3(x, 2.0f, z);
            break;
        }
    }
}

void SceneRenderer::DrawFrame() {
    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    // Barrier: COMMON → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    // Bind RTV + DSV (pipeline owns the depth buffer)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_pipeline.GetDsvHeap()
        ->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    const float clearColor[] = { 0.1f, 0.1f, 0.15f, 1.0f };
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
        1.0f, 0, 0, nullptr);

    // Set viewport
    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sr = { 0, 0, m_width, m_height };
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sr);

    // Compute view-proj
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, (float)m_width / (float)m_height, 0.1f, 1000.0f);

    auto* cam = m_ecs.GetActiveCamera();
    if (cam) {
        for (auto& e : m_ecs.GetEntities()) {
            if (e.camera.get() == cam) {
                XMVECTOR eye = XMLoadFloat3(&e.transform.position);
                XMVECTOR at = XMVectorZero();
                XMVECTOR up = XMVectorSet(0,1,0,0);
                view = XMMatrixLookAtLH(eye, at, up);
                proj = XMMatrixPerspectiveFovLH(
                    cam->fov, (float)m_width / (float)m_height,
                    cam->nearPlane, cam->farPlane);
                break;
            }
        }
    }
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    // Render
    m_pipeline.Render(m_commandList.Get(), m_ecs.GetEntities(), viewProj, clearColor);

    // Barrier: RENDER_TARGET → COMMON
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

} // namespace
