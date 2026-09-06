#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace rex::theme {

enum class ThemeSecurityLevel {
    Declarative,      // JSON/tokens/assets only
    Sandboxed,        // shaders/JS present
    Native,           // DLLs/hooks present (not allowed)
    Unknown
};

struct ThemeManifest {
    std::string themeId;
    std::string name;
    std::string author;
    std::string version;
    int schemaVersion = 1;
    std::vector<std::string> capabilities;   // required features
    std::string description;
};

struct ThemeAsset {
    std::string path;
    std::string type;          // "svg", "png", "font", "shader", ...
    uint64_t sizeBytes = 0;
    bool used = false;         // filled by inspector
};

struct ComponentDeclaration {
    std::string name;          // "taskbar", "dock", ...
    bool enabled = true;
    std::map<std::string, std::string> properties;
};

struct ThemePackage {
    ThemeManifest manifest;
    std::map<std::string, ComponentDeclaration> components;
    std::vector<ThemeAsset> assets;
    ThemeSecurityLevel securityLevel = ThemeSecurityLevel::Unknown;
    std::string originalPath;   // for diagnostics
    // Raw JSON of theme.json
    std::string themeJson;
};

struct CapabilityRequirement {
    std::string capability;
    enum class Status { Supported, Restricted, Unsupported } status = Status::Unsupported;
    std::string fallback;          // if supported with fallback
};

struct AssetReport {
    size_t total = 0;
    // indices: 0 = svg, 1 = png, 2 = jpg, 3 = font, 4 = shader,
    //          5 = json, 6 = text, 7 = other
    size_t byType[8] = {0};
    size_t unused = 0;
    size_t oversized = 0;
    size_t unsupported = 0;
};

struct CompatibilityResult {
    double score = 0.0;          // 0..1
    std::vector<CapabilityRequirement> capabilities;
    std::string summary;
};

struct SecurityReport {
    ThemeSecurityLevel level = ThemeSecurityLevel::Unknown;
    bool hasNativeBinaries = false;
    bool hasExternalScripts = false;
    std::vector<std::string> warnings;
};

struct MigrationReport {
    int sourceSchema = 0;
    int targetSchema = 0;
    bool migrationAvailable = false;
    bool migrationSucceeded = false;
    std::vector<std::string> appliedMigrations;
};

struct ThemeInspectionReport {
    std::string themeId;
    int schemaVersion = 0;
    CompatibilityResult compatibility;
    std::vector<CapabilityRequirement> capabilities;
    AssetReport assets;
    SecurityReport security;
    MigrationReport migration;
    std::vector<std::string> errors;    // parse/validate errors
    bool success = false;
};

} // namespace rex::theme