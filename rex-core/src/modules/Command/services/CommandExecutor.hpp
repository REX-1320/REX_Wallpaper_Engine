#pragma once
#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <utility>
#include <vector>
#include "../models/CommandContext.hpp"
#include "../models/CommandResult.hpp"
#include "../models/CommandDefinition.hpp"

namespace rex::command {

class CommandExecutor {
public:
    // Joins every still-running worker (cancelling it first). Destroying the
    // executor used to leave detached threads running def.execute with `this`
    // — including code that may live in an unloaded plugin DLL — locked
    // against freed memory.
    ~CommandExecutor();

    CommandResult execute(const CommandDefinition& def, CommandContext& ctx,
                          std::function<void(CommandResult)> asyncCallback = nullptr);

    void cancel(const std::string& commandId);

private:
    // Joins workers whose done flag is set (instant) and keeps the list short.
    // Called with mutex_ held.
    void reapFinishedWorkers();

    struct ActiveExecution {
        std::shared_ptr<class SimpleCancellationToken> token;
        std::function<void(CommandResult)> callback;
    };
    std::unordered_map<std::string, ActiveExecution> activeAsync_;
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> workers_;
    std::mutex mutex_;
    std::atomic<bool> shuttingDown_{false};
};

}
