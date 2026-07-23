#pragma once
#include <string>
#include <functional>
#include <unordered_map>

namespace rex::notifications {

enum class AnimationType { Fade, Slide, Scale, None };

struct AnimationState {
    AnimationType type = AnimationType::Fade;
    float progress = 0.0f;
    bool active = false;
    std::function<void()> onFinished;
};

class NotificationAnimation {
public:
    void start(const std::string& id, AnimationType type, float durationMs,
               std::function<void()> onFinished = nullptr);

    void tick(float deltaMs);

    void cancel(const std::string& id);

    float getProgress(const std::string& id) const;

private:
    struct Entry {
        AnimationState state;
        float duration;
        float elapsed = 0.0f;
    };
    std::unordered_map<std::string, Entry> animations_;
};

}
