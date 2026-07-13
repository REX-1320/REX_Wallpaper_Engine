# RES-SDK-Marketplace v1.0

> Depends on: ADR-002, ADR-003, RES-Widget-Platform
> Owns: plugin/widget packaging, CLI, marketplace client + backend contract, permission manifest enforcement, review/signing pipeline

------------------------------------------------------------------------

## 1. Scope

Everything required for a third party to author, package, submit, distribute,
and safely run a widget or plugin. This is the highest-leverage module for
long-term growth (Vision §13) and the highest-risk module for trust/security
(Vision §9) — both concerns are addressed here together, deliberately, because
they are two sides of the same design.

------------------------------------------------------------------------

## 2. Widget/Plugin Embedding Choice

**Decision for Phase 2:** embed a minimal, purpose-built HTML/CSS/JS renderer
(subset — no full DOM/CSS spec compliance required) rather than full Chromium
(CEF) or full Node.js, to keep per-widget process overhead low (ADR-003 assumed
~5–10MB baseline; full CEF is 100MB+ per instance, incompatible with "run 20
widgets without eating a gigabyte of RAM").

**Evaluation checklist for the Phase 2 spike** (this is a real open question,
not fully settled — resolve before committing further SDK design):
- Candidate: `WebView2` in headless/offscreen mode (Microsoft-maintained, real
  Chromium under the hood, but per-instance overhead needs measuring — WebView2
  shares an Edge runtime process pool, may be lighter than raw CEF).
- Candidate: QuickJS (or similar minimal JS engine) + a small custom
  layout/paint engine supporting a CSS subset (flexbox + basic properties only)
  — much lighter, but you build/maintain a mini rendering engine yourself.
- **Recommendation:** start with WebView2 for Phase 2 (fastest to ship, real CSS/HTML
  support, Microsoft-maintained), measure actual per-instance memory with 10+
  concurrent widgets, and only invest in a custom minimal renderer if WebView2's
  overhead genuinely blocks the "lightweight" product thesis in practice.

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
