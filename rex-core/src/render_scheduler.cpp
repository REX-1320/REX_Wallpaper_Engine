#include "render_scheduler.h"
#include <algorithm>

FrameRenderPlan RenderScheduler::PlanFrame(const std::vector<Layer>& layers) {
    FrameRenderPlan plan;

    for (const auto& layer : layers) {
        auto stateIt = m_throttleStates.find(layer.id);
        ThrottleState state = (stateIt != m_throttleStates.end())
            ? stateIt->second : ThrottleState::Normal;

        switch (state) {
        case ThrottleState::Normal:
            plan.fullQualityLayerIds.push_back(layer.id);
            break;
        case ThrottleState::Throttled:
            plan.throttledLayerIds.push_back(layer.id);
            break;
        case ThrottleState::Suspended:
            plan.suspendedLayerIds.push_back(layer.id);
            break;
        }
    }

    return plan;
}

void RenderScheduler::RecordActualRenderTime(uint64_t layerId, double ms) {
    auto& history = m_history[layerId];

    constexpr double alpha = 1.0 / BUDGET_WINDOW_FRAMES;
    history.rollingAverageMs = (1.0 - alpha) * history.rollingAverageMs + alpha * ms;

    bool overBudget = history.rollingAverageMs > PER_WIDGET_DEFAULT_MS;

    if (overBudget) {
        history.consecutiveOverBudget++;
    } else {
        history.consecutiveOverBudget = 0;
        m_throttleStates[layerId] = ThrottleState::Normal;
        return;
    }

    auto currentState = m_throttleStates[layerId];
    if (history.consecutiveOverBudget >= THROTTLE_THRESHOLD) {
        if (currentState == ThrottleState::Normal) {
            m_throttleStates[layerId] = ThrottleState::Throttled;
        } else if (currentState == ThrottleState::Throttled) {
            m_throttleStates[layerId] = ThrottleState::Suspended;
        }
    }
}

ThrottleState RenderScheduler::GetThrottleState(uint64_t layerId) const {
    auto it = m_throttleStates.find(layerId);
    return it != m_throttleStates.end() ? it->second : ThrottleState::Normal;
}
