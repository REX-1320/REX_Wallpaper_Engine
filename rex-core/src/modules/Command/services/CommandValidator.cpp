#include "CommandValidator.hpp"
#include "CommandRegistry.hpp"

namespace rex::command {

bool CommandValidator::canExecute(const std::string& id) const {
    const auto* def = registry_.getDefinition(id);
    if (!def) return false;
    if (def->canExecute) {
        return def->canExecute();
    }
    return true;
}

}
