#pragma once
namespace rex::theme {

// Current REX theme schema version. Packages with a lower schemaVersion
// are candidates for ThemeMigrator upgrades; higher versions are rejected.
inline constexpr int kCurrentThemeSchemaVersion = 1;

} // namespace rex::theme