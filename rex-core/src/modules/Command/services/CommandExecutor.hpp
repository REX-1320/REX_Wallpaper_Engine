#pragma once
#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>
#include "../models/CommandContext.hpp"
#include "../models/CommandResult.hpp"
#include "../models/CommandDefinition.hpp"

namespace rex::command {

class CommandExecutor {
public:
    CommandResult execute(const CommandDefinition& def, CommandContext& ctx,
                          std::function<void(CommandResult)> asyncCallback = nullptr);

    void cancel(const std::string& commandId);

private:
    struct ActiveExecution {
        std::shared_ptr<class SimpleCancellationToken> token;
        std::function<void(CommandResult)> callback;
    };
    std::unordered_map<std::string, ActiveExecution> activeAsync_;
    std::mutex mutex_;
};

}
