#include "ConfigurationStore.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <filesystem>
#endif

namespace rex::modules {

namespace {

// Escape a string for embedding inside a JSON string literal (the settings
// payload is a nested JSON object stored escaped in the config array).
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string jsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u':
                if (i + 4 < s.size()) {
                    out += static_cast<char>(std::stoi(s.substr(i + 1, 4),
                                                       nullptr, 16));
                    i += 4;
                }
                break;
            default: out += s[i];
        }
    }
    return out;
}

} // namespace

static std::string serializeConfigs(const std::vector<ModuleConfig>& configs) {
    std::ostringstream out;
    out << "[\n";
    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        out << "  {\n";
        out << "    \"moduleId\": \"" << cfg.moduleId << "\",\n";
        out << "    \"enabled\": " << (cfg.enabled ? "true" : "false") << ",\n";
        out << "    \"autoStart\": " << (cfg.autoStart ? "true" : "false") << ",\n";
        out << "    \"restartCount\": " << cfg.restartCount;
        if (!cfg.settingsJson.empty()) {
            out << ",\n    \"settings\": \"" << jsonEscape(cfg.settingsJson) << "\"";
        }
        out << "\n  }";
        if (i < configs.size() - 1) out << ",";
        out << "\n";
    }
    out << "]\n";
    return out.str();
}

// Index of the closing quote for the string starting at openQuote+1,
// honoring backslash escapes. Returns npos if unterminated.
static size_t findStringEnd(const std::string& s, size_t openQuote) {
    size_t i = openQuote + 1;
    while (i < s.size()) {
        if (s[i] == '\\') {
            i += 2;
            continue;
        }
        if (s[i] == '"') return i;
        ++i;
    }
    return std::string::npos;
}

// Split a top-level array into its brace-delimited objects. Braces inside
// quoted strings (e.g. an escaped settings payload) are ignored.
static std::vector<std::string> splitObjects(const std::string& text) {
    std::vector<std::string> objs;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '{') {
            ++i;
            continue;
        }
        size_t start = i;
        bool inString = false;
        size_t j = start + 1;
        for (; j < text.size(); ++j) {
            char c = text[j];
            if (inString) {
                if (c == '\\') {
                    ++j;
                    continue;
                }
                if (c == '"') inString = false;
            } else if (c == '"') {
                inString = true;
            } else if (c == '}') {
                break;
            }
        }
        if (j >= text.size()) break;
        objs.push_back(text.substr(start, j - start + 1));
        i = j + 1;
    }
    return objs;
}

static std::vector<ModuleConfig> deserializeConfigs(const std::string& text) {
    std::vector<ModuleConfig> result;
    for (const auto& obj : splitObjects(text)) {
        ModuleConfig cfg;

        auto findField = [&](const std::string& key) -> std::string {
            auto kpos = obj.find('"' + key + '"');
            if (kpos == std::string::npos) return {};
            auto colon = obj.find(':', kpos + key.size() + 2);
            if (colon == std::string::npos) return {};
            auto vpos = colon + 1;
            while (vpos < obj.size() && (obj[vpos] == ' ' || obj[vpos] == '\t')) ++vpos;
            if (vpos < obj.size() && obj[vpos] == '"') {
                auto vend = findStringEnd(obj, vpos);
                if (vend == std::string::npos) return {};
                return obj.substr(vpos + 1, vend - vpos - 1);
            }
            auto vend = obj.find_first_of(",}", vpos);
            if (vend == std::string::npos) vend = obj.size();
            return obj.substr(vpos, vend - vpos);
        };

        auto findBool = [&](const std::string& key, bool def) -> bool {
            auto kpos = obj.find('"' + key + '"');
            if (kpos == std::string::npos) return def;
            auto vpos = obj.find(':', kpos);
            if (vpos == std::string::npos) return def;
            ++vpos;
            while (vpos < obj.size() && obj[vpos] == ' ') ++vpos;
            if (obj.substr(vpos, 4) == "true") return true;
            if (obj.substr(vpos, 5) == "false") return false;
            return def;
        };

        auto findInt = [&](const std::string& key, int def) -> int {
            auto kpos = obj.find('"' + key + '"');
            if (kpos == std::string::npos) return def;
            auto vpos = obj.find(':', kpos);
            if (vpos == std::string::npos) return def;
            ++vpos;
            while (vpos < obj.size() && obj[vpos] == ' ') ++vpos;
            int sign = 1;
            if (vpos < obj.size() && obj[vpos] == '-') { sign = -1; ++vpos; }
            int val = 0;
            while (vpos < obj.size() && obj[vpos] >= '0' && obj[vpos] <= '9') {
                val = val * 10 + (obj[vpos] - '0');
                ++vpos;
            }
            return val * sign;
        };

        cfg.moduleId = findField("moduleId");
        if (cfg.moduleId.empty()) continue;
        cfg.enabled = findBool("enabled", true);
        cfg.autoStart = findBool("autoStart", false);
        cfg.restartCount = findInt("restartCount", 0);
        cfg.settingsJson = jsonUnescape(findField("settings"));

        result.push_back(cfg);
    }
    return result;
}

ConfigurationStore::ConfigurationStore(const std::string& filePath)
    : filePath_(filePath) {}

std::vector<ModuleConfig> ConfigurationStore::loadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadAllUnlocked();
}

// Unlocked reader: only valid with mutex_ held (see saveModule), or from
// contexts that already serialized access.
std::vector<ModuleConfig> ConfigurationStore::loadAllUnlocked() {
    std::ifstream file(filePath_);
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return deserializeConfigs(buffer.str());
}

void ConfigurationStore::saveAll(const std::vector<ModuleConfig>& configs) {
    std::lock_guard<std::mutex> lock(mutex_);
    saveAllUnlocked(configs);
}

// Atomic write: serialize to a sibling temp file and rename over the target
// in one step. The old code truncated modules.json in place, so a crash or a
// power cut mid-write left a half-written file — and loadAll treats a corrupt
// file exactly like a fresh install (all settings gone).
void ConfigurationStore::saveAllUnlocked(const std::vector<ModuleConfig>& configs) {
    const std::string payload = serializeConfigs(configs);
    const std::string tmpPath = filePath_ + ".tmp";
    {
        std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot write config file: " + tmpPath);
        file << payload;
        file.flush();
        if (!file) throw std::runtime_error("Failed writing config file: " + tmpPath);
    }
#ifdef _WIN32
    // MOVEFILE_REPLACE_EXISTING replaces the target atomically on the same
    // volume (MOVEFILE_WRITE_THROUGH forces the metadata to disk).
    if (!MoveFileExA(tmpPath.c_str(), filePath_.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace config file: " + filePath_);
    }
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::rename(tmpPath, filePath_, ec);
    if (ec) throw std::runtime_error("Cannot replace config file: " + filePath_);
#endif
}

ModuleConfig ConfigurationStore::loadModule(const std::string& moduleId) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Unlocked variant: loadAll() would re-lock mutex_ on this same thread
    // (non-recursive) and throw "resource deadlock would occur".
    const auto all = loadAllUnlocked();
    for (auto& cfg : all) {
        if (cfg.moduleId == moduleId) return cfg;
    }
    return ModuleConfig{moduleId};
}

void ConfigurationStore::saveModule(const ModuleConfig& config) {
    // Held across the whole read-modify-write: the old code released the lock
    // between loadAll and saveAll, so two threads racing enableModule /
    // persistSetting silently destroyed each other's entries.
    std::lock_guard<std::mutex> lock(mutex_);
    auto all = loadAllUnlocked();
    bool found = false;
    for (auto& cfg : all) {
        if (cfg.moduleId == config.moduleId) {
            cfg = config;
            found = true;
            break;
        }
    }
    if (!found) all.push_back(config);
    saveAllUnlocked(all);
}

} // namespace rex::modules
