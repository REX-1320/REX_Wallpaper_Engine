#pragma once
#include <string>
#include <any>

namespace rex::search {
struct SearchFilter {
    enum class Type { Category, Module, Tag, DateRange, Custom };
    Type type;
    std::string value;
    std::any customFilter;
};
}
