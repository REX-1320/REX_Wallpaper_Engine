#pragma once

namespace rex::command {

class ICommandService;

class ICommandProvider {
public:
    virtual ~ICommandProvider() = default;
    virtual void registerCommands(ICommandService& service) = 0;
};

}
