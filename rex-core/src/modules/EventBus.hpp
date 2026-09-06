#pragma once
#include <functional>
#include <vector>
#include <mutex>
#include "ModuleEvents.hpp"

namespace rex::modules {

class EventBus {
public:
    using Callback = std::function<void(const ModuleEvent&)>;

    void subscribe(Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.push_back(std::move(cb));
    }

    virtual void publish(const ModuleEvent& event) {
        // Snapshot under lock, invoke WITHOUT the lock: subscribers may
        // subscribe()/publish() re-entrantly (previously self-deadlocked on
        // std::mutex and inverted lock order with module mutexes).
        std::vector<Callback> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = subscribers_;
        }
        for (auto& cb : snapshot) {
            if (cb) cb(event);
        }
    }

private:
    std::vector<Callback> subscribers_;
    std::mutex mutex_;
};

} // namespace rex::modules
