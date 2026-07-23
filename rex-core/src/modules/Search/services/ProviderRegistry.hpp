#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include "../interfaces/ISearchProvider.hpp"

namespace rex::search {

class ProviderRegistry {
public:
    bool add(ISearchProvider* provider);
    bool remove(const std::string& id);
    std::vector<ISearchProvider*> getAll() const;
    ISearchProvider* get(const std::string& id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ISearchProvider*> providers_;
};

}
