#include "CommandExecutor.hpp"
#include <thread>

namespace rex::command {

class SimpleCancellationToken : public ICancellationToken {
public:
    bool isCancelled() const override { return cancelled_.load(); }
    void cancel() { cancelled_.store(true); }
private:
    std::atomic<bool> cancelled_{false};
};

CommandExecutor::~CommandExecutor() {
    shuttingDown_.store(true, std::memory_order_release);

    // Ask every in-flight execution to stop, then wait for the workers. A
    // detached thread could otherwise outlive the executor (and the module /
    // DLL that owns it) and touch freed memory in `def.execute` or `mutex_`.
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> owned;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, active] : activeAsync_) {
            if (active.token) active.token->cancel();
        }
        owned = std::move(workers_);
    }
    for (auto& [thread, done] : owned) {
        if (thread.joinable()) thread.join();
    }
}

void CommandExecutor::reapFinishedWorkers() {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->second->load(std::memory_order_acquire) && it->first.joinable()) {
            it->first.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

CommandResult CommandExecutor::execute(const CommandDefinition& def, CommandContext& ctx,
                                       std::function<void(CommandResult)> asyncCallback) {
    if (def.mode == ExecutionMode::Immediate) {
        return def.execute(ctx);
    } else if (def.mode == ExecutionMode::Background) {
        auto token = std::make_shared<SimpleCancellationToken>();
        ctx.cancelToken = token.get();

        auto done = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard lock(mutex_);
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return CommandResult{false, "Executor is shutting down"};
            }
            reapFinishedWorkers();
            ActiveExecution active;
            active.token = token;
            active.callback = asyncCallback;
            activeAsync_[def.id] = std::move(active);
        }

        std::thread worker([this, def, ctx = std::move(ctx), asyncCallback, done]() mutable {
            auto ctxCopy = std::make_shared<CommandContext>(std::move(ctx));
            CommandResult result = def.execute(*ctxCopy);
            bool cancelled = false;
            {
                std::lock_guard lock(mutex_);
                // cancel() erases the entry: only report results that were
                // not cancelled. The old code invoked the success callback
                // even for cancelled commands (double-reporting through the
                // dispatcher) and consulted neither the map nor
                // def.supportsCancellation.
                cancelled = (activeAsync_.find(def.id) == activeAsync_.end());
                if (!cancelled) activeAsync_.erase(def.id);
            }
            if (!cancelled && asyncCallback) {
                asyncCallback(result);
            }
            done->store(true, std::memory_order_release);
        });
        {
            std::lock_guard lock(mutex_);
            workers_.emplace_back(std::move(worker), done);
        }
        return CommandResult{true, "", {}};
    }
    return CommandResult{false, "Unsupported execution mode"};
}

void CommandExecutor::cancel(const std::string& commandId) {
    std::lock_guard lock(mutex_);
    auto it = activeAsync_.find(commandId);
    if (it != activeAsync_.end()) {
        if (it->second.token) {
            it->second.token->cancel();
        }
        activeAsync_.erase(it);
    }
}

}
