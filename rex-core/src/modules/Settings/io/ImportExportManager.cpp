#include "ImportExportManager.hpp"
#include "../interfaces/ISettingsService.hpp"
#include <algorithm>
#include <cctype>

namespace rex::settings {

ImportExportManager::ImportExportManager(SettingsRegistry& registry, ValidationEngine& validator)
    : registry_(registry), validator_(validator) {}

std::string ImportExportManager::serializeValue(const SettingValue& val) {
    if (std::holds_alternative<bool>(val)) {
        return std::get<bool>(val) ? "true" : "false";
    }
    if (std::holds_alternative<int64_t>(val)) {
        return std::to_string(std::get<int64_t>(val));
    }
    if (std::holds_alternative<double>(val)) {
        return std::to_string(std::get<double>(val));
    }
    if (std::holds_alternative<std::string>(val)) {
        std::string s = std::get<std::string>(val);
        std::string escaped;
        for (char c : s) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        return "\"" + escaped + "\"";
    }
    if (std::holds_alternative<std::vector<std::string>>(val)) {
        std::string result = "[";
        bool first = true;
        for (const auto& v : std::get<std::vector<std::string>>(val)) {
            if (!first) result += ",";
            result += "\"" + v + "\"";
            first = false;
        }
        result += "]";
        return result;
    }
    if (std::holds_alternative<std::vector<uint8_t>>(val)) {
        return "\"<binary>\"";
    }
    return "null";
}

std::string ImportExportManager::exportToJson(const std::vector<std::string>& settingIds) const {
    std::ostringstream out;
    out << "[\n";
    auto settings = settingIds.empty() ? registry_.getAllSettings()
                   : [&]() {
                       std::vector<Setting> res;
                       for (const auto& id : settingIds) {
                           const Setting* s = registry_.getSetting(id);
                           if (s) res.push_back(*s);
                       }
                       return res;
                   }();
    bool first = true;
    for (const auto& s : settings) {
        if (!first) out << ",\n";
        out << "  {\n";
        out << "    \"id\": \"" << s.id << "\",\n";
        out << "    \"type\": " << static_cast<int>(s.type) << ",\n";
        out << "    \"value\": " << serializeValue(s.currentValue) << "\n";
        out << "  }";
        first = false;
    }
    out << "\n]\n";
    return out.str();
}

bool ImportExportManager::deserializeValue(const std::string& str, SettingType type, SettingValue& out) {
    auto trim = [](const std::string& s) -> std::string {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
        return s.substr(start, end - start);
    };
    std::string t = trim(str);

    switch (type) {
        case SettingType::Boolean:
            if (t == "true") { out = true; return true; }
            if (t == "false") { out = false; return true; }
            return false;
        case SettingType::Integer:
            try { out = static_cast<int64_t>(std::stoll(t)); return true; }
            catch (...) { return false; }
        case SettingType::Float:
            try { out = std::stod(t); return true; }
            catch (...) { return false; }
        case SettingType::String:
        case SettingType::Path:
        case SettingType::File:
        case SettingType::Directory:
        case SettingType::Enum: {
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
                out = t.substr(1, t.size() - 2);
                return true;
            }
            out = t;
            return true;
        }
        default:
            return false;
    }
}

bool ImportExportManager::importFromJson(const std::string& json, bool overwrite, ISettingsService& service) {
    auto findField = [](const std::string& line, const std::string& field) -> std::string {
        auto pos = line.find('"' + field + '"');
        if (pos == std::string::npos) return {};
        auto colon = line.find(':', pos);
        if (colon == std::string::npos) return {};
        auto valStart = line.find_first_not_of(" \t", colon + 1);
        if (valStart == std::string::npos) return {};
        std::string rest = line.substr(valStart);
        if (rest.front() == '"') {
            auto end = rest.find('"', 1);
            if (end == std::string::npos) return {};
            return rest.substr(1, end - 1);
        }
        auto comma = rest.find(',');
        auto brace = rest.find('}');
        auto end = std::string::npos;
        if (comma != std::string::npos && brace != std::string::npos)
            end = std::min(comma, brace);
        else if (comma != std::string::npos)
            end = comma;
        else if (brace != std::string::npos)
            end = brace;
        return rest.substr(0, end);
    };

    std::istringstream stream(json);
    std::string line;
    std::string currentId, currentValue;
    int currentType = -1;
    bool inObject = false;

    while (std::getline(stream, line)) {
        auto trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        if (trimmed.empty() || trimmed == "[" || trimmed == "]") continue;

        if (trimmed == "{") { inObject = true; currentId.clear(); currentValue.clear(); currentType = -1; continue; }
        if (trimmed == "}," || trimmed == "}") {
            inObject = false;
            if (currentId.empty() || currentType < 0) continue;
            const Setting* existing = registry_.getSetting(currentId);
            if (existing && !overwrite) continue;

            SettingValue parsedVal;
            if (!deserializeValue(currentValue, static_cast<SettingType>(currentType), parsedVal)) continue;

            if (existing) {
                ValidationResult vr = validator_.validate(*existing, parsedVal);
                if (!vr.valid) return false;
                service.setValue(currentId, parsedVal);
            }
            continue;
        }

        if (trimmed.find("\"id\"") != std::string::npos) {
            currentId = findField(trimmed, "id");
        } else if (trimmed.find("\"type\"") != std::string::npos) {
            auto val = findField(trimmed, "type");
            if (!val.empty()) currentType = std::stoi(val);
        } else if (trimmed.find("\"value\"") != std::string::npos) {
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                currentValue = trimmed.substr(colon + 1);
                auto comma = currentValue.find_last_of(",}");
                if (comma != std::string::npos) currentValue = currentValue.substr(0, comma);
            }
        }
    }
    return true;
}

}
