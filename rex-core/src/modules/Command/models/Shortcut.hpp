#pragma once
#include <cstdint>
#include <string>

namespace rex::command {

struct Shortcut {
    uint32_t modifiers = 0;
    uint32_t keyCode = 0;

    bool isValid() const { return keyCode != 0; }
    bool operator==(const Shortcut& other) const {
        return modifiers == other.modifiers && keyCode == other.keyCode;
    }
};

}
