#include "core/scheduler/render_scheduler.h"
#include <algorithm>
#include <cstdio>

FrameRenderPlan RenderScheduler::PlanFrame(const std::vector<Layer>& layers) {
    FrameRenderPlan plan;

    for (const auto& layer : layers) {
        auto stateIt = m_throttleStates.find(layer.id);
        ThrottleState state = (stateIt != m_throttleStates.end())
            ? stateIt->second : ThrottleState::Normal;

        auto& history = m_history[layer.id];

        switch (state) {
        case ThrottleState::Normal:
            plan.fullQualityLayerIds.push_back(layer.id);
            break;
        case ThrottleState::Throttled:
            plan.throttledLayerIds.push_back(layer.id);
            break;
        case ThrottleState::Suspended:
            // Periodic trial frame for recovery check
            history.framesSinceLastTrial++;
            if (history.framesSinceLastTrial >= TRIAL_FRAME_INTERVAL) {
                plan.fullQualityLayerIds.push_back(layer.id);
                history.framesSinceLastTrial = 0;
                printf("[REX] RenderScheduler: Trial frame for layer %llu\n", layer.id);
            } else {
                plan.suspendedLayerIds.push_back(layer.id);
            }
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
        history.consecutiveInBudget = 0;
    } else {
        history.consecutiveInBudget++;
        history.consecutiveOverBudget = 0;

        // Recover if sustained in-budget performance
        if (history.consecutiveInBudget >= RECOVERY_THRESHOLD) {
            auto state = m_throttleStates[layerId];
            if (state == ThrottleState::Suspended) {
                m_throttleStates[layerId] = ThrottleState::Throttled;
                printf("[REX] RenderScheduler: Layer %llu recovered from Suspended to Throttled\n", layerId);
            } else if (state == ThrottleState::Throttled) {
                m_throttleStates[layerId] = ThrottleState::Normal;
                printf("[REX] RenderScheduler: Layer %llu fully recovered\n", layerId);
            }
        }
        return;
    }

    auto& currentState = m_throttleStates[layerId];
    if (history.consecutiveOverBudget >= THROTTLE_THRESHOLD) {
        if (currentState == ThrottleState::Normal) {
            m_throttleStates[layerId] = ThrottleState::Throttled;
            printf("[REX] RenderScheduler: Layer %llu throttled\n", layerId);
        } else if (currentState == ThrottleState::Throttled) {
            m_throttleStates[layerId] = ThrottleState::Suspended;
            printf("[REX] RenderScheduler: Layer %llu suspended\n", layerId);
        }
    }
}

ThrottleState RenderScheduler::GetThrottleState(uint64_t layerId) const {
    auto it = m_throttleStates.find(layerId);
    return it != m_throttleStates.end() ? it->second : ThrottleState::Normal;
}
