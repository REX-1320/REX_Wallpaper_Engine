#pragma once
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <stdexcept>

namespace rex::modules {

class ServiceContainer {
public:
    template<typename T>
    void registerService(std::shared_ptr<T> service) {
        services_[std::type_index(typeid(T))] = std::move(service);
    }

    template<typename T>
    std::shared_ptr<T> getService() const {
        auto it = services_.find(std::type_index(typeid(T)));
        if (it != services_.end()) {
            return std::static_pointer_cast<T>(it->second);
        }
        throw std::runtime_error("Service not found");
    }

    template<typename T>
    bool hasService() const {
        return services_.find(std::type_index(typeid(T))) != services_.end();
    }

    std::shared_ptr<void> findService(const std::type_index& type) const {
        auto it = services_.find(type);
        if (it != services_.end()) return it->second;
        return nullptr;
    }

private:
    std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
};

} // namespace rex::modules
