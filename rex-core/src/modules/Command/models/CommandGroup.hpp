#pragma once
#include <string>
#include <vector>

namespace rex::command {

struct CommandGroup {
    std::string id;
    std::string name;
    std::string icon;
    int order = 0;
    std::vector<std::string> children;
};

}
