# RES-SDK-Marketplace v1.1

> Depends on: ADR-002, ADR-003, RES-Widget-Platform
> Owns: plugin/widget packaging, CLI, marketplace client + backend contract, permission manifest enforcement, review/signing pipeline

**Changelog:**
- v1.1 (Phase 1): Resolved rendering choice — WebView2 for Phase 1, minimal renderer re-evaluated at Phase 2 entry. Added §2.1 with integration spec.

------------------------------------------------------------------------

## 1. Scope

Everything required for a third party to author, package, submit, distribute,
and safely run a widget or plugin. This is the highest-leverage module for
long-term growth (Vision §13) and the highest-risk module for trust/security
(Vision §9) — both concerns are addressed here together, deliberately, because
they are two sides of the same design.

------------------------------------------------------------------------

## 2. Widget/Plugin Embedding Choice

### 2.1 Phase 1 Decision — WebView2 Offscreen Mode

**Status:** Accepted for Phase 1

Embed Microsoft Edge WebView2 in offscreen/headless mode as the widget rendering
engine. Rationale:

- **Fastest path to a working widget** — WebView2 is pre-installed on Windows 11
  and available via the Evergreen bootstrapper on Windows 10. Zero custom rendering
  engine to build or maintain during Phase 1.
- **Real CSS/HTML/JS support** — widget authors write standard web code. No subset
  limitations, no "why doesn't flexbox work" support burden during MVP.
- **Process-pool sharing** — WebView2 instances share a single `edge_video.exe`
  child process across all widgets in the same app. Actual per-widget memory
  overhead is ~5–15MB (not the 100MB+ of isolated CEF), meeting ADR-003's
  assumption.
- **DComposition integration** — `CreateCoreWebView2CompositionController` provides
  a `DCompositionTarget` that can be integrated with the Core's D3D12 compositor
  via shared DXGI resources.

**Integration approach for Phase 1:**

```
Widget process (rex-widget-host.exe)
  ├── Hidden WS_POPUP window (message pump)
  ├── WebView2 environment (single per-process)
  ├── CoreWebView2 offscreen controller
  │     └── Composition target → D3D12 shared texture
  ├── IPC client → Shell (named pipe, per-widget)
  └── Shared memory handle → Core compositor
```

- Each widget process creates one WebView2 instance in offscreen mode.
- The web content renders into a DComposition visual whose output is copied to
  a shared D3D12 texture (via `ID3D11Device` interop or `IDXGIResource` sharing).
- The texture handle is sent to the Core over the control IPC channel
  (`RegisterWidgetSurface` message in `rex.proto`).
- The Core compositor imports the handle and composites the texture as a layer.

**Phase 1 scope limitation:** shared-memory texture handoff uses a CPU copy
(WebView2 offscreen → system memory → GPU texture) rather than direct GPU-GPU
handoff. This adds ~1–3ms per widget per frame — acceptable for Phase 1 widget
counts (<10 widgets). Direct GPU sharing is a Phase 2 optimization.

**Requirement validated by Phase 0 sandbox PoC:** Job Object capping + process
isolation per widget means a WebView2 crash (Edge runtime bug, OOM) kills only
one widget, not the desktop.

### 2.2 Phase 2 Re-evaluation — Minimal Renderer

The long-term goal (ADR-003, v1.0 of this doc) remains a minimal JS + layout
engine to drop per-widget overhead below 5MB. The Phase 2 spike resolves:

- **Candidate A (continue):** WebView2 — if Phase 1 shows acceptable overhead
  at 20+ concurrent widgets on target hardware (8GB RAM), skip the custom
  renderer entirely.
- **Candidate B (build):** QuickJS + custom flexbox layout engine — if WebView2
  overhead genuinely blocks the "lightweight" product thesis. Estimated 6–8 weeks
  of engine work alone; must be scheduled explicitly in Phase 2, not discovered
  mid-phase.

**Decision gate:** at Phase 1 exit, measure and document:
- Peak working set of Shell + Core + N widget processes at N = 5, 10, 20
- Per-widget frame-time cost of the CPU copy texture path
- User-visible lag at N = 10 widgets with the CPU copy path

------------------------------------------------------------------------

## 3. CLI Tooling

```
rex-cli init <widget-name>          # scaffolds manifest.json + boilerplate
rex-cli dev                         # local dev server, hot-reload into a running REX instance
rex-cli validate                    # schema + permission manifest lint
rex-cli package                     # produces a signed-ready .rexwidget zip
rex-cli publish                     # submits to marketplace review queue (Phase 5)
```

`rex-cli dev` connects to a running REX instance over a dedicated dev-mode named
pipe, pushing file changes to a live widget process without a full reload —
this is the single highest-value piece of tooling for your own dogfooding
(Vision §10 "invest early in your own developer loop") and should be built
before you write your third first-party widget, not after.

------------------------------------------------------------------------

## 4. Data Source Plugins (extending beyond widgets)

Third-party data sources (Vision §5 Widget Platform note) implement the same
`IDataSource` contract as first-party ones (RES-Widget-Platform §2.3), packaged
similarly but with a distinct manifest `type: "datasource"`. They run in the
same sandboxed process model as widgets — a malicious or buggy data source
plugin has the same blast-radius controls as a widget.

------------------------------------------------------------------------

## 5. Permission Manifest — Full Schema

```json
{
  "permissions": {
    "network": { "requested": true, "domains": ["api.openweathermap.org"] },
    "filesystem": { "requested": false },
    "dataSources": ["weather"],
    "systemInfo": ["cpu", "ram"],
    "notifications": false
  }
}
```

**Enforcement points (defense in depth, not a single check):**
1. **Install time:** user sees a human-readable permission summary, must accept explicitly (not a wall of checkboxes — see UX note below).
2. **Sandbox process spawn:** Job Object + process-level restrictions applied matching only the *granted* permissions, not merely the *requested* ones.
3. **Runtime:** `window.rex` API surface conditionally exposes `network`/`storage` etc. based on grants — an ungranted API simply doesn't exist in the widget's JS context, it doesn't fail with a permission error (smaller attack surface, nothing to probe).

**UX requirement:** permission prompts must name *why* in plain language
("This widget wants to check the weather from api.openweathermap.org"), not
just list a permission enum — this is a trust feature, not a formality.

------------------------------------------------------------------------

## 6. Code Signing & Review Pipeline (Phase 5)

```
Developer submits .rexwidget
        │
        ▼
Automated scan: manifest schema valid? permissions declared match actual
API calls used (static analysis of the JS)? known-malware signature scan?
        │
        ▼ (pass)
Queued for lightweight human review (you, initially — solo maintained)
        │
        ▼ (approved)
REX signs the package (Ed25519 signature over package hash)
        │
        ▼
Published to marketplace index; client verifies signature before install
```

**Solo-maintainer reality check:** at launch, "human review" is you. Budget for
this explicitly — it does not scale past a certain submission volume without
either (a) trusted-community reviewers with limited review permissions, or (b)
tightening the automated scan so more submissions pass without manual eyes.
Do not let review-queue backlog silently become the thing that kills momentum —
track time-to-review as a named metric (Vision §16).

**Static analysis scan (minimum viable, Phase 5):**
- Flag any use of `eval`, dynamic `Function()` construction, or obfuscated code — hold for manual review, don't auto-reject (may be a false positive) but don't auto-approve either.
- Flag network calls to domains not declared in the manifest.
- Flag any attempt to access Node-style APIs (`require`, `process`, `fs`) that shouldn't exist in the sandbox at all — presence of these calls indicates either a sandbox escape attempt or a broken assumption about the runtime, both worth flagging.

------------------------------------------------------------------------

## 7. Versioning & Compatibility

- SDK API versioned independently of the app (`rex-sdk@2.1.0`), following semver.
- A widget manifest declares `"sdkVersion": "^2.0.0"` — the Shell refuses to load
  a widget requesting an incompatible major version rather than attempting
  best-effort compatibility, which prevents silent subtle breakage.
- Deprecation policy: a minor SDK version may mark an API deprecated (console
  warning surfaced to the widget developer in `rex-cli dev`); removal only on a
  major version bump, minimum one full major-version cycle of deprecation notice.

------------------------------------------------------------------------

## 8. Marketplace Backend Contract (minimum viable, Phase 5)

```
GET  /api/v1/widgets?query=&category=&sort=        # search/browse
GET  /api/v1/widgets/{id}                           # detail + version list
POST /api/v1/widgets/{id}/submit                    # new version submission (authenticated)
GET  /api/v1/widgets/{id}/download/{version}        # signed package download
```

Minimum viable backend: a REST API + a package blob store (even flat-file
storage is fine at launch scale) + a Postgres/SQLite table for metadata. This
does not need to be elaborate at Phase 5 — resist over-building it before
there's real submission volume to justify more.

------------------------------------------------------------------------

## 9. Testing Requirements

- Golden-path test: scaffold → dev-mode hot reload → package → (mock) publish, verified end-to-end as a single CI job — this is the Phase 2 exit criteria from the Vision doc ("a stranger builds a working widget from docs alone") made testable.
- Adversarial test fixtures: a widget attempting undeclared network access, a widget attempting filesystem escape, a widget deliberately exceeding its resource quota — all must be caught by the sandbox, not by the review pipeline (defense in depth means these tests target enforcement, not detection).
- Signature verification tests: tampered package must fail client-side verification before install, not just fail server-side.
