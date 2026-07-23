#pragma once
#include <string>
#include <any>

namespace rex::command {

struct CommandResult {
    bool success = true;
    std::string errorMessage;
    std::any data;
};

}
