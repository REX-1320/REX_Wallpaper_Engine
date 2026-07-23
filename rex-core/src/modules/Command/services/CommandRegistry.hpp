#pragma once
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include "../models/CommandDefinition.hpp"
#include "../models/CommandInstance.hpp"

namespace rex::command {

class CommandRegistry {
public:
    bool addDefinition(const CommandDefinition& def);
    bool removeDefinition(const std::string& id);
    const CommandDefinition* getDefinition(const std::string& id) const;
    std::vector<CommandInstance> getAllInstances() const;

    bool setInstanceEnabled(const std::string& id, bool enabled);
    bool setInstanceVisible(const std::string& id, bool visible);
    const CommandInstance* getInstance(const std::string& id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CommandDefinition> definitions_;
    std::unordered_map<std::string, CommandInstance> instances_;

    void syncInstance(const std::string& id);
};

}
