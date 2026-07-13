#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "compositor.h"

enum class ThrottleState { Normal, Throttled, Suspended };

struct TimingHistory {
    double rollingAverageMs = 0.0;
    uint32_t consecutiveOverBudget = 0;
    uint32_t consecutiveInBudget = 0;
    uint32_t framesSinceLastTrial = 0;
};

struct FrameRenderPlan {
    std::vector<uint64_t> fullQualityLayerIds;
    std::vector<uint64_t> throttledLayerIds;
    std::vector<uint64_t> suspendedLayerIds;
};

class RenderScheduler {
public:
    static constexpr double TOTAL_FRAME_MS = 16.6;
    static constexpr double COMPOSITOR_RESERVED_MS = 2.0;
    static constexpr double PER_WIDGET_DEFAULT_MS = 2.0;
    static constexpr uint32_t BUDGET_WINDOW_FRAMES = 30;
    static constexpr uint32_t THROTTLE_THRESHOLD = 60;
    static constexpr uint32_t TRIAL_FRAME_INTERVAL = 600; // ~10s at 60fps
    static constexpr uint32_t RECOVERY_THRESHOLD = 60;

    FrameRenderPlan PlanFrame(const std::vector<Layer>& layers);
    void RecordActualRenderTime(uint64_t layerId, double ms);
    ThrottleState GetThrottleState(uint64_t layerId) const;

private:
    std::unordered_map<uint64_t, TimingHistory> m_history;
    std::unordered_map<uint64_t, ThrottleState> m_throttleStates;
};
