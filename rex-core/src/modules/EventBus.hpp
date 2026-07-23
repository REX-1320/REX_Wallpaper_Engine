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
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& cb : subscribers_) {
            if (cb) cb(event);
        }
    }

private:
    std::vector<Callback> subscribers_;
    std::mutex mutex_;
};

} // namespace rex::modules
