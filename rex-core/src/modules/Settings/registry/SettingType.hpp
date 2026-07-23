#pragma once
#include <string>

namespace rex::settings {

enum class SettingType {
    Boolean,
    Integer,
    Float,
    String,
    Color,
    Path,
    File,
    Directory,
    Enum,
    Hotkey,
    Json,
    List,
    Object
};

inline const char* toString(SettingType type) {
    switch (type) {
        case SettingType::Boolean:   return "Boolean";
        case SettingType::Integer:   return "Integer";
        case SettingType::Float:     return "Float";
        case SettingType::String:    return "String";
        case SettingType::Color:     return "Color";
        case SettingType::Path:      return "Path";
        case SettingType::File:      return "File";
        case SettingType::Directory: return "Directory";
        case SettingType::Enum:      return "Enum";
        case SettingType::Hotkey:    return "Hotkey";
        case SettingType::Json:      return "Json";
        case SettingType::List:      return "List";
        case SettingType::Object:    return "Object";
    }
    return "Unknown";
}

} // namespace rex::settings
