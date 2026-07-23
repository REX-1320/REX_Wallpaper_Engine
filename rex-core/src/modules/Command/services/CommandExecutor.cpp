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

CommandResult CommandExecutor::execute(const CommandDefinition& def, CommandContext& ctx,
                                       std::function<void(CommandResult)> asyncCallback) {
    if (def.mode == ExecutionMode::Immediate) {
        return def.execute(ctx);
    } else if (def.mode == ExecutionMode::Background) {
        auto token = std::make_shared<SimpleCancellationToken>();
        ctx.cancelToken = token.get();

        {
            std::lock_guard lock(mutex_);
            ActiveExecution active;
            active.token = token;
            active.callback = asyncCallback;
            activeAsync_[def.id] = std::move(active);
        }

        std::thread worker([this, def, ctx = std::move(ctx), asyncCallback]() mutable {
            auto ctxCopy = std::make_shared<CommandContext>(std::move(ctx));
            CommandResult result = def.execute(*ctxCopy);
            if (asyncCallback) {
                asyncCallback(result);
            }
            {
                std::lock_guard lock(mutex_);
                activeAsync_.erase(def.id);
            }
        });
        worker.detach();
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
