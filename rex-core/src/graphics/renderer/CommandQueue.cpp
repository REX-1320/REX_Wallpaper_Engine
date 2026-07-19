#include "CommandQueue.hpp"
#include <shared/Logger.hpp>

namespace rex::graphics {

CommandQueue::~CommandQueue() {
    Shutdown();
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

    m_fenceValue = 0;
    REX_LOG_INFO("CommandQueue", "Shutdown");
}

void CommandQueue::Execute(ID3D12CommandList* const* lists, UINT count) {
    m_queue->ExecuteCommandLists(count, lists);
}

void CommandQueue::Execute(ID3D12CommandList* list) {
    ID3D12CommandList* lists[] = { list };
    m_queue->ExecuteCommandLists(1, lists);
}

void CommandQueue::WaitForGPU() {
    m_fenceValue++;
    HRESULT hr = m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (FAILED(hr)) {
        REX_LOG_ERROR("CommandQueue", "Signal failed: {:08x}", hr);
        return;
    }

    if (m_fence->GetCompletedValue() < m_fenceValue) {
        hr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
}

UINT64 CommandQueue::Signal() {
    m_fenceValue++;
    HRESULT hr = m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (FAILED(hr)) {
        REX_LOG_ERROR("CommandQueue", "Signal failed: {:08x}", hr);
    }
    return m_fenceValue;
}

} // namespace rex::graphics
