# RES-Testing-Security-Performance v1.0

> Depends on: all module RES documents. Cross-cutting concerns that apply
> across the whole codebase.

------------------------------------------------------------------------

# Part A: Testing Strategy

## A.1 Test Pyramid

```
        ▲  Manual test matrix (multi-monitor, Win10/11, DPI scaling)
       ╱ ╲   — small in count, high in value, run before each release
      ╱   ╲
     ╱     ╲ Integration tests (real IPC, real sandboxed processes,
    ╱       ╲  real WorkerW acquisition on CI Windows runners)
   ╱         ╲
  ╱           ╲ Unit tests (C# xUnit, C++ Catch2)
 ╱_____________╲  — the majority of test count, fast, run on every commit
```

## A.2 CI Pipeline (solo-dev-appropriate, not enterprise-heavyweight)

**Every commit (push to any branch):**
1. Build C++ core (CMake, Debug + Release configs)
2. Build C# shell (`dotnet build`)
3. Run unit tests both sides
4. Lint (clang-tidy for C++, `dotnet format --verify-no-changes` for C#)

**On merge to main:**
1. All of the above
2. Integration test suite (requires a Windows runner — GitHub Actions `windows-latest` is sufficient for Phase 0–2; revisit if self-hosted runners become necessary for GPU-dependent tests)
3. Package a dev build artifact

**Nightly (once Phase 1+ is real):**
1. Full test suite including the adversarial sandbox fixtures (RES-Widget-Platform §8, RES-SDK-Marketplace §9)
2. Performance regression check against the SLAs in Part C below

**Solo-dev rationale:** this replaces the code-review safety net a team would
give you for free (Vision §10). Do not skip CI "because it's just you" —
that's precisely when you need it most.

## A.3 What Must Never Ship Without a Test

- Any change to the IPC protocol (`.proto` schema) — a breaking change here is silent and catastrophic (ADR-002).
- Any change to sandbox enforcement (Job Object limits, permission gating) — regressions here are security regressions (ADR-003).
- Any change to profile/widget-layout serialization — must include a fixture of the *previous* schema version to catch migration breakage (ADR-004).

## A.4 Fixture Widgets (Permanent Test Assets)

Maintain these forever, do not delete after initial development:
- `hello-world-widget` — minimal valid widget, smoke test for the whole pipeline.
- `misbehaving-widget-infinite-loop` — validates CPU quota enforcement.
- `misbehaving-widget-memory-bomb` — validates memory quota enforcement.
- `misbehaving-widget-network-escape` — attempts undeclared network access, validates permission enforcement.
- `misbehaving-widget-fs-escape` — attempts filesystem access outside sandbox storage.

------------------------------------------------------------------------

# Part B: Security Model

## B.1 Threat Model (what REX defends against)

| Threat actor | Concern | Primary mitigation |
|---|---|---|
| Malicious widget author | Data exfiltration, resource abuse, desktop instability | Sandboxing (ADR-003), permission manifest (RES-SDK-Marketplace §5) |
| Compromised widget dependency (supply chain) | Malicious code introduced post-review via an updated dependency | Static scan re-run on every version submission, not just first submission |
| Man-in-the-middle on package download | Tampered widget package | Ed25519 signature verification client-side (RES-SDK-Marketplace §6) |
| Local privilege escalation via widget process | Widget process attempts to escape sandbox to gain broader system access | Job Object restrictions + no elevated token ever passed to widget processes + regular adversarial fixture testing (§A.4) |

**Explicitly out of scope for REX's threat model:** REX is not defending
against a fully compromised Windows install, a malicious Administrator user
account, or physical access attacks — standard OS security boundary
assumptions apply.

## B.2 Defense in Depth (why enforcement happens at multiple layers)

Per RES-SDK-Marketplace §5, permission enforcement happens at install-time
(disclosure), spawn-time (process restriction), and runtime (API surface
absence). This is deliberate redundancy — a bug in any single layer should not
be a full sandbox escape. When adding a new permission-gated capability in the
future, implement all three layers, not just one.

## B.3 Secrets & Signing Key Management

- The marketplace signing private key (Ed25519, ADR/RES-SDK-Marketplace §6) is
  the single most sensitive asset in the whole project — compromise means an
  attacker can sign malicious packages that install as "verified."
- Store it in a hardware-backed store or, at minimum, an encrypted secret not
  committed to any repository, with access limited to the release/signing CI
  job only.
- Have a key-rotation plan documented before Phase 5 launch, not after — revoking
  a compromised key with no rotation plan in place is a fire you don't want to be fighting live.

## B.4 Incident Response (solo-dev-scaled)

- A documented (even if brief) response plan: how to pull a malicious package
  from the marketplace index within minutes, how to push a client-side
  blocklist for an already-downloaded malicious package, who/where users are
  notified (Discord/forum per Vision §13).
- This does not need to be elaborate — it needs to exist *before* Phase 5
  launch, written down, not improvised during an actual incident.

------------------------------------------------------------------------

# Part C: Performance SLAs (Concrete Numbers)

These are targets, not laws — but they should be measured against continuously
(§A.2 nightly performance regression check), not treated as aspirational text.

| Metric | Target | Rationale |
|---|---|---|
| App cold start to first widget rendered | < 2 seconds | Vision §8 "fast startup" made concrete |
| Idle CPU usage (5 widgets, no interaction) | < 2% | Competitive with/better than Rainmeter baseline |
| Idle RAM usage (5 widgets) | < 250MB total (Shell + Core + 5 widget processes) | Justifies "lightweight" product positioning against Electron-based competitors |
| Per-widget frame budget | 2ms render time per frame at 60fps target (16.6ms total frame budget shared across all active widgets + compositor overhead) | Enforced by the render scheduler (RES-Wallpaper-WindowFX-Automation-Rendering §4) |
| IPC round-trip latency (control channel) | < 1ms local named pipe | Validated in Phase 0 Step 2 |
| Widget crash-to-notification latency | < 500ms | User should never wonder "did that widget just silently die" |
| Profile switch time (10 widgets) | < 1 second perceived (may suspend/resume rather than full teardown, RES-Desktop-Platform §4) | |

**Benchmark discipline:** re-run the Phase 3 exit criteria benchmark
("side-by-side performance vs. Rainmeter/Wallpaper Engine at idle," Vision §16)
every time a Core rendering change lands, not just once before launch.

------------------------------------------------------------------------

# Part D: Definition of Done (applies to every module)

A feature is not "done" until:
1. Unit tests pass (and exist).
2. It's covered by the CI pipeline (§A.2).
3. If it touches IPC, sandboxing, or serialization — the corresponding §A.3 "never ship without a test" checklist item is satisfied.
4. It's measured against relevant Part C SLAs if performance-relevant.
5. It's documented (even briefly) in the relevant RES doc if it changes that module's contract.
