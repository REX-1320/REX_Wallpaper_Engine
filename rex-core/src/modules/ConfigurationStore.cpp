#include "ConfigurationStore.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>

namespace rex::modules {

static std::string serializeConfigs(const std::vector<ModuleConfig>& configs) {
    std::ostringstream out;
    out << "[\n";
    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& cfg = configs[i];
        out << "  {\n";
        out << "    \"moduleId\": \"" << cfg.moduleId << "\",\n";
        out << "    \"enabled\": " << (cfg.enabled ? "true" : "false") << ",\n";
        out << "    \"autoStart\": " << (cfg.autoStart ? "true" : "false") << ",\n";
        out << "    \"restartCount\": " << cfg.restartCount << "\n";
        out << "  }";
        if (i < configs.size() - 1) out << ",";
        out << "\n";
    }
    out << "]\n";
    return out.str();
}

static std::vector<ModuleConfig> deserializeConfigs(const std::string& text) {
    std::vector<ModuleConfig> result;
    size_t pos = 0;
    while (true) {
        auto objStart = text.find('{', pos);
        if (objStart == std::string::npos) break;
        auto objEnd = text.find('}', objStart);
        if (objEnd == std::string::npos) break;

        std::string obj = text.substr(objStart, objEnd - objStart + 1);
        pos = objEnd + 1;

        ModuleConfig cfg;
        auto findField = [&](const std::string& key) -> std::string {
            auto kpos = obj.find('"' + key + '"');
            if (kpos == std::string::npos) return {};
            auto vstart = obj.find('"', kpos + key.size() + 3);
            if (vstart == std::string::npos) return {};
            ++vstart;
            auto vend = obj.find('"', vstart);
            if (vend == std::string::npos) return {};
            return obj.substr(vstart, vend - vstart);
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

        result.push_back(cfg);
    }
    return result;
}

ConfigurationStore::ConfigurationStore(const std::string& filePath)
    : filePath_(filePath) {}

std::vector<ModuleConfig> ConfigurationStore::loadAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream file(filePath_);
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    return deserializeConfigs(buffer.str());
}

void ConfigurationStore::saveAll(const std::vector<ModuleConfig>& configs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(filePath_);
    if (!file) throw std::runtime_error("Cannot write config file: " + filePath_);
    file << serializeConfigs(configs);
}

ModuleConfig ConfigurationStore::loadModule(const std::string& moduleId) {
    auto all = loadAll();
    for (auto& cfg : all) {
        if (cfg.moduleId == moduleId) return cfg;
    }
    return ModuleConfig{moduleId};
}

void ConfigurationStore::saveModule(const ModuleConfig& config) {
    auto all = loadAll();
    bool found = false;
    for (auto& cfg : all) {
        if (cfg.moduleId == config.moduleId) {
            cfg = config;
            found = true;
            break;
        }
    }
    if (!found) all.push_back(config);
    saveAll(all);
}

} // namespace rex::modules
