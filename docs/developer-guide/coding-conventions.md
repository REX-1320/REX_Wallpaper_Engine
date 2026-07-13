# REX Tools — Coding Conventions

**Enforced by:** CI (`cargo clippy`, `cargo fmt`, `dotnet format`)

---

## General

- Write for future-you. Six months from now, you're a stranger.
- Prefer boring, obvious code. Novelty budget goes to rendering/compositing.
- One module at a time. Don't fix bugs outside your current scope.
- AI-generated code must be reviewed on architecture, security, and public API.

## Rust

**Naming:**

| Convention | Example |
|---|---|
| `snake_case` for functions, variables | `fn create_widget_host()` |
| `PascalCase` for types, traits, enums | `struct WidgetInstance` |
| `SCREAMING_SNAKE_CASE` for constants | `const MAX_WIDGETS: u32` |

**Error handling:**
- `thiserror` for library error types. `anyhow` for binary code.
- Return `Result<_, Error>` from fallible functions. No `unwrap()` in library code.
- `expect()` only in tests and `main()`, with a descriptive message.
- Use `anyhow::Context` for error chaining.

```rust
fn load_widget(path: &Path) -> Result<WidgetManifest, WidgetError> {
    let content = fs::read_to_string(path)
        .with_context(|| format!("Failed to read {}", path.display()))?;
    serde_json::from_str(&content).map_err(WidgetError::from)
}
```

**Testing:**
- Unit tests in `#[cfg(test)] mod tests` in the same file.
- Integration tests in `tests/` directory.
- Property-based tests with `proptest` for IPC serialization, data parsing.

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_manifest_parsing() -> Result<(), WidgetError> {
        let json = r#"{"id": "test", "name": "Test", "version": "1.0.0"}"#;
        let m: WidgetManifest = serde_json::from_str(json)?;
        assert_eq!(m.name, "Test");
        Ok(())
    }
}
```

**Concurrency:** Prefer channels over shared state. Use `parking_lot::Mutex` over `std::sync::Mutex`. Mark `Send + Sync` explicitly.

## C#

**Naming:**

| Convention | Example |
|---|---|
| `PascalCase` for types, methods, properties | `WidgetManager`, `GetWidgetById()` |
| `camelCase` for locals, parameters | `widgetId` |
| `_camelCase` for private fields | `_widgetManager` |
| Interfaces prefixed with `I` | `IWidgetHost` |

**Async:**
- `async Task` for async methods (not `async void`).
- `ConfigureAwait(false)` in library code, not UI handlers.
- `Async` suffix: `LoadWidgetAsync()`.

```csharp
public async Task<WidgetManifest> LoadWidgetAsync(string path)
{
    var content = await File.ReadAllTextAsync(path).ConfigureAwait(false);
    return JsonSerializer.Deserialize<WidgetManifest>(content);
}
```

**MVVM:**
- Views (XAML) bind to ViewModels. ViewModels never reference Views.
- `ObservableCollection<T>` for bound collections.
- `INotifyPropertyChanged` via base class.
- `RelayCommand` (CommunityToolkit.Mvvm).
- Services injected via constructor (Microsoft.Extensions.DependencyInjection).

**Testing:** xUnit + Moq/NSubstitute + FluentAssertions.

```csharp
[Fact]
public async Task LoadWidget_ValidPath_ReturnsManifest()
{
    var service = new WidgetService();
    var result = await service.LoadWidgetAsync("test_data/valid_widget");
    result.Should().NotBeNull();
    result.Name.Should().Be("Test Widget");
}
```

## Git

| Convention | Rule |
|---|---|
| Branch naming | `phase-N/short-desc` or `fix/short-desc` |
| Commit messages | Conventional Commits: `type(scope): description` |
| Types | `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `security` |
| Scope | `core`, `shell`, `sdk`, `proto`, `docs` |
| Body | Explain *why*, not *what* |

Example: `feat(core): implement WorkerW re-attachment on explorer restart`

## Code Review Checklist (Solo)

Before merging to main:

- [ ] Compiles without warnings?
- [ ] All tests pass?
- [ ] New tests for new code?
- [ ] Public API documented?
- [ ] No `unwrap()` or `expect()` in library code?
- [ ] No TODO/FIXME comments?
- [ ] IPC protocol changed? Proto schema updated?
- [ ] New permission added? Reviewed for necessity?
- [ ] Error handling for all fallible operations?
- [ ] Performance-critical code benchmarked?
- [ ] Works on Windows 10 and 11?
