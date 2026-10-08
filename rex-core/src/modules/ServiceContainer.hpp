#pragma once
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <stdexcept>

namespace rex::modules {

class ServiceContainer {
public:
    // Services are registered while modules initialize and resolved from the
    // IPC / update threads, so the map needs a lock — an unordered_map
    // rehash during a concurrent find is undefined behaviour.
    template<typename T>
    void registerService(std::shared_ptr<T> service) {
        std::lock_guard<std::mutex> lock(mutex_);
        services_[std::type_index(typeid(T))] = std::move(service);
    }

    template<typename T>
    std::shared_ptr<T> getService() const {
        auto svc = findService(std::type_index(typeid(T)));
        if (!svc) throw std::runtime_error("Service not found");
        return std::static_pointer_cast<T>(svc);
    }

    template<typename T>
    bool hasService() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return services_.find(std::type_index(typeid(T))) != services_.end();
    }

    std::shared_ptr<void> findService(const std::type_index& type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(type);
        if (it != services_.end()) return it->second;
        return nullptr;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
};

} // namespace rex::modules
