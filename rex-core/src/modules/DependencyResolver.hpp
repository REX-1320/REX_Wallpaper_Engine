#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rex::modules {

class DependencyResolver {
public:
    struct ModuleInfo {
        std::string id;
        std::vector<std::string> dependencies;
    };

    std::vector<std::string> resolveOrder(const std::vector<ModuleInfo>& modules) const;

private:
    bool hasCycle(const std::unordered_map<std::string, std::vector<std::string>>& adj,
                  const std::string& node, std::unordered_set<std::string>& visited,
                  std::unordered_set<std::string>& recStack) const;
};

} // namespace rex::modules
