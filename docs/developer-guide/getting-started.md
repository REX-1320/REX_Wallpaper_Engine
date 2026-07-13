# REX Tools — Developer Onboarding Guide

---

## 1. Prerequisites

| Tool | Version | Check |
|---|---|---|
| Rust toolchain | stable (2024+) | `rustc --version` |
| .NET SDK | 8.0+ | `dotnet --version` |
| Git | 2.40+ | `git --version` |
| Windows 10 SDK | 10.0.20348.0+ | (included with VS Build Tools) |
| protoc | 25+ | `protoc --version` |

```powershell
winget install Rustlang.Rustup Google.Protobuf Microsoft.DotNet.SDK.8
rustup default stable
rustup component add clippy rustfmt
```

Recommended: Visual Studio 2022+ (C#/WinUI), VS Code (Rust), GitHub CLI.

## 2. Clone & Build

```powershell
git clone <repo-url> rex
cd rex

# Build all
cd core && cargo build
cd ../shell && dotnet build
cd ../proto && protoc --rust_out=../core/rex-ipc/src --csharp_out=../shell/REX.Shell.Services/Proto rex.proto
```

## 3. Run

```powershell
# Terminal 1: core
cd core && cargo run

# Terminal 2: shell
cd shell && dotnet run
```

## 4. Run Tests

```powershell
# Core
cd core && cargo test

# Shell
cd shell && dotnet test

# All
cd core && cargo test; if ($?) { cd ..\shell && dotnet test }
```

## 5. Common Workflows

**Add a new IPC message:**
1. Edit `proto/rex.proto` — add message type + fields.
2. Run proto code generation.
3. Implement handler in `core/rex-core/src/ipc.rs`.
4. Implement caller in `shell/REX.Shell.Services/IpcClient.cs`.
5. Add tests on both sides.

**Create a new widget (Phase 2+):**
```powershell
rex new widget my-weather-widget
cd my-weather-widget
rex dev
```

**Add a new data source:**
1. Implement `DataSource` trait in `core/rex-data/src/`.
2. Register in data source factory.
3. Add to IPC schema if new event type.
4. Update widget API docs.

## 6. Debugging

**Core crash:** Check `%LOCALAPPDATA%\REX\crashes\` for minidumps. Analyze with WinDbg.

**IPC connection failure:** Verify core is running (`Get-Process rex-core`). Check pipe name `\\.\pipe\REX\core\v1`. Firewall does not affect named pipes.

**Widget not rendering:** Check WebView2 runtime installed. Verify widget path exists. Set `REX_WEBVIEW2_DEBUG=1`.

**Environment variables:**

| Variable | Default | Purpose |
|---|---|---|
| `RUST_LOG` | `info` | Log level: trace, debug, info, warn, error |
| `REX_IPC_TRACE` | (unset) | Log all IPC messages when `1` |
| `REX_SKIP_WORKERW` | (unset) | Run without desktop attachment |
| `REX_DEV_MODE` | (unset) | Dev features (DevTools, inspector) |
| `REX_PROFILE` | `default` | Load specific profile on startup |
| `REX_WEBVIEW2_DEBUG` | (unset) | Show WebView2 DevTools when `1` |
