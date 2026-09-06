#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <utility>

namespace rex::graphics {

// Tracks D3D12 resource states so barriers are only inserted when necessary.
// Assumes single-threaded command recording (render thread).
class ResourceStateTracker {
public:
    // Transition a resource to a new state on a specific subresource.
    // Returns true if a barrier was inserted (state actually changed).
    bool Transition(ID3D12GraphicsCommandList* cmdList,
                    ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES newState,
                    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        auto key = std::make_pair(resource, subresource);
        auto it = m_states.find(key);
        D3D12_RESOURCE_STATES oldState =
            (it != m_states.end()) ? it->second : D3D12_RESOURCE_STATE_COMMON;

        if (oldState == newState) {
            return false;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = resource;
        barrier.Transition.StateBefore = oldState;
        barrier.Transition.StateAfter  = newState;
        barrier.Transition.Subresource = subresource;
        cmdList->ResourceBarrier(1, &barrier);

        m_states[key] = newState;
        return true;
    }

    // Reset all tracked states (call after command list reset).
    void Reset() { m_states.clear(); }

private:
    struct PairHash {
        size_t operator()(const std::pair<ID3D12Resource*, UINT>& p) const noexcept {
            size_t h1 = std::hash<ID3D12Resource*>{}(p.first);
            size_t h2 = std::hash<UINT>{}(p.second);
            return h1 ^ (h2 << 1);
        }
    };

    std::unordered_map<std::pair<ID3D12Resource*, UINT>,
                       D3D12_RESOURCE_STATES,
                       PairHash> m_states;
};

} // namespace rex::graphics
