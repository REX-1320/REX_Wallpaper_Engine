#include "ProviderRegistry.hpp"

namespace rex::search {

bool ProviderRegistry::add(ISearchProvider* provider) {
    std::lock_guard lock(mutex_);
    auto meta = provider->getMetadata();
    if (providers_.count(meta.id)) return false;
    providers_[meta.id] = provider;
    return true;
}

bool ProviderRegistry::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    return providers_.erase(id) > 0;
}

std::vector<ISearchProvider*> ProviderRegistry::getAll() const {
    std::lock_guard lock(mutex_);
    std::vector<ISearchProvider*> result;
    for (auto& [id, p] : providers_) result.push_back(p);
    return result;
}

ISearchProvider* ProviderRegistry::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = providers_.find(id);
    return it != providers_.end() ? it->second : nullptr;
}

}
