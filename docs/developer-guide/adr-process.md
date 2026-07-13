# ADR Process

---

## What is an ADR?

An Architecture Decision Record documents a decision that is:

- **Irreversible** — changing your mind later is expensive.
- **Consequential** — affects multiple modules, public API, or process.
- **Non-obvious** — someone might reasonably choose differently.

Reversible or isolated decisions go in a comment.

## When to Write

| Decision | ADR? |
|---|---|
| Stack (Rust vs C++ vs C#) | Yes |
| IPC transport (named pipes vs gRPC) | Yes |
| Plugin sandbox model | Yes |
| Widget rendering approach | Yes |
| Config format (JSON5 vs TOML) | No |
| Which HTTP crate | No |
| Indentation style | No (use formatter) |

## Format

File: `docs/architecture/adr/ADR-NNN-title.md`

```markdown
# ADR-{NNN}: {Title}

**Status:** {Proposed | Accepted | Deprecated | Superseded by ADR-{NNN}}
**Date:** {YYYY-MM-DD}
**Author:** {Name}

## Context

What is the issue? What constraints apply?

## Decision

What are we doing? State in one sentence, then elaborate.

## Rationale

Why this over alternatives? Include evidence or benchmarks.

## Consequences

**Positive:** Benefit 1, Benefit 2.
**Negative:** Trade-off 1, Trade-off 2.
**Neutral:** Implication that's neither good nor bad.

## Alternatives Considered

### Alternative 1 — why rejected
### Alternative 2 — why rejected

## Related ADRs

- ADR-{NNN} — relationship

## References

- Links to docs, issues, external resources
```

## Lifecycle

```
Proposed → Accepted → (if needed) Deprecated
```

**Proposed:** Written, not yet implemented. Wait 24h (solo: sleep on it).
**Accepted:** Decision final. Begin implementation.
**Deprecated:** No longer applies. Reference superseding ADR.

## Solo Review Process

1. Write draft.
2. Wait 24 hours.
3. Review with fresh eyes: "Would I still decide this?"
4. If yes: mark Accepted. Begin implementation.
5. If unsure: prototype the riskiest aspect (time-box: 1 week) before finalizing.

## Current Registry

| ADR | Title | Status | Date |
|---|---|---|---|
| ADR-001 | Tech Stack & Architecture Decision | Decided | 2026-07-13 |
