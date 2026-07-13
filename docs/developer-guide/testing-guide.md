# REX Tools — Testing Guide

---

## Philosophy

For a solo developer, tests are the safety net a team provides. Every regression caught by a test is a bug you don't debug at 2 AM.

- Test behavior, not implementation.
- Write tests alongside code (red-green-refactor).
- If it can break, test it.
- If it's trivial (getter, simple delegation), don't.

## Test Pyramid

```
        ╱╲
       ╱ E2E ╲          Few: core + shell together
      ╱────────╲
     ╱Integration╲       Some: module boundaries (IPC, compositor)
    ╱──────────────╲
   ╱   Unit tests    ╲   Many: isolated functions and types
  ╱────────────────────╲
```

**Unit tests:** Single function/type in isolation. Mock external deps. Coverage: >80% for core logic (manifest parsing, layout math, permission checks).

**Integration tests:** Cross-module within same language. Core: data source → event bus → IPC serialization. Shell: IPC deserialization → widget manager → ViewModel. Cross-language: Rust IPC server + C# IPC client.

**E2E tests:** Full system — core + shell + WorkerW surface. Run on CI only (headless, slow). Verify core starts, compositor creates surface, IPC works.

## Rust Tests

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_widget_manifest_parses() {
        let json = r#"{"id":"a","name":"b","version":"1.0","permissions":[],"entry":"i.html"}"#;
        let m: WidgetManifest = serde_json::from_str(json).unwrap();
        assert_eq!(m.id, "a");
    }

    #[test]
    fn test_invalid_manifest_fails() {
        assert!(serde_json::from_str::<WidgetManifest>(r#"{"id":123}"#).is_err());
    }
}
```

Property-based tests for round-trip data:

```rust
proptest! {
    #[test]
    fn test_ipc_roundtrip(id in "[a-z]{1,32}", x in 0i32..3840, y in 0i32..2160) {
        let msg = WidgetCommand::Move { id, x, y };
        let bytes = msg.to_bytes();
        assert_eq!(msg, WidgetCommand::from_bytes(&bytes).unwrap());
    }
}
```

## C# Tests

```csharp
[Fact]
public void Parse_ValidJson_ReturnsManifest()
{
    var json = @"{""id"":""a"",""name"":""b"",""version"":""1.0""}";
    var m = WidgetManifest.Parse(json);
    m.Id.Should().Be("a");
}

[Theory]
[InlineData("{}")]
[InlineData("null")]
public void Parse_InvalidJson_Throws(string json)
{
    Action act = () => WidgetManifest.Parse(json);
    act.Should().Throw<WidgetManifestException>();
}
```

## Running Tests

```powershell
# Core
cd core && cargo test

# With output
cd core && cargo test -- --nocapture

# Single test
cd core && cargo test test_widget_manifest_parses

# Shell
cd shell && dotnet test

# Single C# test
cd shell && dotnet test --filter "FullyQualifiedName~WidgetManifestTests"
```

## CI (GitHub Actions)

```yaml
jobs:
  test-core:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: dtolnay/rust-toolchain@stable
      - run: cargo build --manifest-path core/Cargo.toml
      - run: cargo test --manifest-path core/Cargo.toml
      - run: cargo clippy --manifest-path core/Cargo.toml -- -D warnings

  test-shell:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-dotnet@v4
        with: { dotnet-version: '8.0.x' }
      - run: dotnet build shell/REX.sln
      - run: dotnet test shell/REX.sln
      - run: dotnet format shell/REX.sln --verify-no-changes
```

## Testing Checklist (Each PR)

- [ ] New public API has unit tests.
- [ ] New IPC message has round-trip test.
- [ ] Error paths tested (invalid input, missing data, permission denied).
- [ ] Tests not flaky (run 3x locally).
- [ ] CI passes (build + test + lint).
- [ ] If perf-critical: benchmark before and after (3 runs, report average).
