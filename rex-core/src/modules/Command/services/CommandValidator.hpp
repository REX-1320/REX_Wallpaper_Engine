#pragma once
#include <string>

namespace rex::command {

class CommandRegistry;

class CommandValidator {
public:
    explicit CommandValidator(const CommandRegistry& registry) : registry_(registry) {}

    bool canExecute(const std::string& id) const;

private:
    const CommandRegistry& registry_;
};

}
