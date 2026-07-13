#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

enum class EasingFunction {
    Linear,
    EaseInQuad, EaseOutQuad, EaseInOutCubic,
    EaseInSine, EaseOutSine, EaseInOutSine
};

struct Animation {
    double durationMs;
    EasingFunction easing;
    std::function<void(double progress)> onTick;
    std::function<void()> onComplete;
    double elapsedMs = 0.0;
    bool completed = false;
};

using AnimationHandle = uint64_t;

class AnimationEngine {
public:
    AnimationHandle Start(Animation anim);
    void Cancel(AnimationHandle handle);
    void Tick(double frameTimeMs);

private:
    static double ApplyEasing(EasingFunction fn, double t);

    std::unordered_map<AnimationHandle, Animation> m_animations;
    AnimationHandle m_nextHandle = 1;
};
