#pragma once
#include <any>
#include <string>

namespace rex::modules { class EventBus; }

namespace rex::command {

class ICommandService;

class ICancellationToken {
public:
    virtual bool isCancelled() const = 0;
    virtual ~ICancellationToken() = default;
};

struct CommandContext {
    std::string source;
    std::any parameters;
    ICancellationToken* cancelToken = nullptr;
    ICommandService* service = nullptr;
    rex::modules::EventBus* bus = nullptr;
};

}
