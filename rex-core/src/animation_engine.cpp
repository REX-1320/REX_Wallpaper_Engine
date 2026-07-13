#define _USE_MATH_DEFINES
#include "animation_engine.h"
#include <cmath>
#include <algorithm>

AnimationHandle AnimationEngine::Start(Animation anim) {
    AnimationHandle handle = m_nextHandle++;
    anim.completed = false;
    anim.elapsedMs = 0.0;
    m_animations[handle] = std::move(anim);
    return handle;
}

void AnimationEngine::Cancel(AnimationHandle handle) {
    m_animations.erase(handle);
}

void AnimationEngine::Tick(double frameTimeMs) {
    AnimationHandle toRemove[64];
    int removeCount = 0;

    for (auto& [handle, anim] : m_animations) {
        anim.elapsedMs += frameTimeMs;
        double t = std::min(anim.elapsedMs / anim.durationMs, 1.0);
        double progress = ApplyEasing(anim.easing, t);

        if (anim.onTick) {
            anim.onTick(progress);
        }

        if (t >= 1.0 && removeCount < 64) {
            anim.completed = true;
            if (anim.onComplete) anim.onComplete();
            toRemove[removeCount++] = handle;
        }
    }

    for (int i = 0; i < removeCount; i++) {
        m_animations.erase(toRemove[i]);
    }
}

double AnimationEngine::ApplyEasing(EasingFunction fn, double t) {
    switch (fn) {
    case EasingFunction::Linear:
        return t;
    case EasingFunction::EaseInQuad:
        return t * t;
    case EasingFunction::EaseOutQuad:
        return t * (2.0 - t);
    case EasingFunction::EaseInOutCubic:
        return t < 0.5 ? 4.0 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
    case EasingFunction::EaseInSine:
        return 1.0 - cos((t * M_PI) / 2.0);
    case EasingFunction::EaseOutSine:
        return sin((t * M_PI) / 2.0);
    case EasingFunction::EaseInOutSine:
        return -(cos(M_PI * t) - 1.0) / 2.0;
    default:
        return t;
    }
}
