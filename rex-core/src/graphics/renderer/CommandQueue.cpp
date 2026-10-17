#include "CommandQueue.hpp"
#include <shared/Logger.hpp>
#include <exception>

namespace rex::graphics {

CommandQueue::~CommandQueue() {
    try {
        Shutdown();
    } catch (const std::exception& ex) {
        REX_LOG_ERROR("CommandQueue", "~CommandQueue exception: {}", ex.what());
    } catch (...) {
        REX_LOG_ERROR("CommandQueue", "~CommandQueue unknown exception");
    }
}

shared::Result<void> CommandQueue::Initialize(ID3D12Device* device) {
    if (m_queue) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "CommandQueue already initialized");
    }
    if (!device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Device is null");
    }

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateCommandQueue failed");
    }

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateFence failed");
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateEvent failed");
    }

    REX_LOG_INFO("CommandQueue", "Initialized");
    return shared::Result<void>();
}

void CommandQueue::Shutdown() {
    if (m_queue) {
        WaitForGPU();
    }

    m_queue.Reset();
    m_fence.Reset();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_fenceMutex);
        m_fenceValue = 0;
    }
}

void CommandQueue::Execute(ID3D12CommandList* const* lists, UINT count) {
    m_queue->ExecuteCommandLists(count, lists);
}

void CommandQueue::Execute(ID3D12CommandList* list) {
    ID3D12CommandList* lists[] = { list };
    m_queue->ExecuteCommandLists(1, lists);
}

void CommandQueue::WaitForGPU() {
    if (!m_queue || !m_fence) return;

    UINT64 value = 0;
    {
        // R2: increment + Signal as one unit — two threads signaling the
        // shared queue could otherwise order Signal(v) after Signal(v+1),
        // dropping the fence value backwards and hanging waiters.
        std::lock_guard<std::mutex> lock(m_fenceMutex);
        value = ++m_fenceValue;
        HRESULT hr = m_queue->Signal(m_fence.Get(), value);
        if (FAILED(hr)) {
            REX_LOG_ERROR("CommandQueue", "Signal failed: {:08x}", hr);
            return;
        }
    }

    if (m_fence->GetCompletedValue() < value) {
        // R2: per-wait event — the shared auto-reset m_fenceEvent loses
        // wakeups when two waiters race (one reset consumes the signal).
        HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!ev) return;
        if (SUCCEEDED(m_fence->SetEventOnCompletion(value, ev))) {
            WaitForSingleObject(ev, INFINITE);
        }
        CloseHandle(ev);
    }
}

void CommandQueue::WaitForFence(ID3D12Fence* fence, UINT64 value) {
    if (!m_queue || !fence) return;
    m_queue->Wait(fence, value);
}

UINT64 CommandQueue::Signal() {
    if (!m_queue || !m_fence) return 0;
    std::lock_guard<std::mutex> lock(m_fenceMutex);
    const UINT64 value = ++m_fenceValue;
    HRESULT hr = m_queue->Signal(m_fence.Get(), value);
    if (FAILED(hr)) {
        REX_LOG_ERROR("CommandQueue", "Signal failed: {:08x}", hr);
    }
    return value;
}

bool CommandQueue::WaitForValue(UINT64 value, DWORD timeoutMs) {
    if (!m_queue || !m_fence) return false;
    if (m_fence->GetCompletedValue() >= value) return true;
    // Dedicated event (not m_fenceEvent): WaitForGPU may use that one
    // concurrently from another thread, and sharing an auto-reset event
    // across two waiters loses wakeups.
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return false;
    HRESULT hr = m_fence->SetEventOnCompletion(value, ev);
    bool reached = SUCCEEDED(hr) &&
                   WaitForSingleObject(ev, timeoutMs) == WAIT_OBJECT_0;
    if (FAILED(hr)) {
        REX_LOG_ERROR("CommandQueue", "SetEventOnCompletion failed: {:08x}", hr);
    }
    CloseHandle(ev);
    return reached;
}

} // namespace rex::graphics
