#include "DependencyResolver.hpp"
#include <algorithm>
#include <stdexcept>
#include <queue>

namespace rex::modules {

std::vector<std::string> DependencyResolver::resolveOrder(const std::vector<ModuleInfo>& modules) const {
    std::unordered_map<std::string, std::vector<std::string>> adj;
    std::unordered_map<std::string, int> indegree;
    std::unordered_set<std::string> allNodes;

    for (const auto& mod : modules) {
        allNodes.insert(mod.id);
        indegree[mod.id] = 0;
    }

    for (const auto& mod : modules) {
        for (const auto& dep : mod.dependencies) {
            if (allNodes.find(dep) == allNodes.end())
                continue;
            adj[dep].push_back(mod.id);
            indegree[mod.id]++;
        }
    }

    std::queue<std::string> q;
    for (const auto& node : allNodes) {
        if (indegree[node] == 0)
            q.push(node);
    }

    std::vector<std::string> order;
    while (!q.empty()) {
        std::string u = q.front(); q.pop();
        order.push_back(u);
        for (const auto& v : adj[u]) {
            if (--indegree[v] == 0)
                q.push(v);
        }
    }

    if (order.size() != allNodes.size()) {
        throw std::runtime_error("Circular dependency detected among modules");
    }
    return order;
}

} // namespace rex::modules
