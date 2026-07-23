#include "NotificationAnimation.hpp"

namespace rex::notifications {

void NotificationAnimation::start(const std::string& id, AnimationType type,
                                  float durationMs, std::function<void()> onFinished) {
    Entry entry;
    entry.state.type = type;
    entry.state.active = true;
    entry.duration = durationMs;
    entry.state.onFinished = std::move(onFinished);
    animations_[id] = entry;
}

void NotificationAnimation::tick(float deltaMs) {
    std::vector<std::string> finished;
    for (auto& [id, entry] : animations_) {
        if (!entry.state.active) continue;
        entry.elapsed += deltaMs;
        float prog = entry.elapsed / entry.duration;
        if (prog >= 1.0f) {
            prog = 1.0f;
            entry.state.active = false;
            if (entry.state.onFinished) entry.state.onFinished();
            finished.push_back(id);
        }
        entry.state.progress = prog;
    }
    for (const auto& id : finished) animations_.erase(id);
}

void NotificationAnimation::cancel(const std::string& id) {
    animations_.erase(id);
}

float NotificationAnimation::getProgress(const std::string& id) const {
    auto it = animations_.find(id);
    if (it != animations_.end()) return it->second.state.progress;
    return 1.0f;
}

}
