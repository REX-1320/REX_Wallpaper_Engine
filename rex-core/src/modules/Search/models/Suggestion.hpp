#pragma once
#include <string>

namespace rex::search {
struct Suggestion {
    std::string text;
    std::string category;
    double weight = 0.0;
};
}
