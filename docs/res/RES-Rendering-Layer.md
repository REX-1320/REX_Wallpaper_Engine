# RES-Rendering-Layer v1.0

> Depends on: ADR-001, ADR-002, ADR-003, RES-Widget-Platform §4, RES-Wallpaper-Platform §3, RES-WindowFX §4
> Owns: GPU compositor, layer model, render scheduler / frame budget enforcement, animation engine

------------------------------------------------------------------------

## 1. Scope

The Rendering Layer is the Core's (C++) single point of GPU composition —
every visual surface from every other module (widget textures, wallpaper
frames, WindowFX overlays) is submitted here as a layer and composited in one
coordinated frame. This module is disproportionately important relative to
its line count: it's the enforcement point for "one bad plugin can't tank the
desktop" (Vision §5), the highest-test-coverage-priority component in the Core.

------------------------------------------------------------------------

## 2. Layer Model

```cpp
enum class LayerKind { Wallpaper, Widget, WindowFxOverlay };

struct Layer {
    LayerId id;
    LayerKind kind;
    ComPtr<ID3D12Resource> texture;      // shared or owned, depending on source
    D2D_RECT_F bounds;
    float opacity;
    int zOrder;
    bool visible;
};

class Compositor {
public:
    void SubmitLayer(const Layer& layer);
    void RemoveLayer(LayerId id);
    void RenderFrame();
private:
    std::vector<Layer> _layers;   // sorted by zOrder before each composite pass
};
```

**Z-order convention (lowest to highest):** Wallpaper → Widgets (by their own
declared `ZOrder`, RES-Widget-Platform §2.2) → WindowFX overlays (shadows/
animation overlays render above their target window but conceptually "own"
layer space, not competing with widget z-order). This ordering is a fixed
architectural rule, not per-frame configurable — changing it would require an ADR.

------------------------------------------------------------------------

## 3. Render Scheduler / Frame Budget Enforcement

This is the concrete implementation of the "render budget" concept from the
Vision doc and the enforcement point referenced by RES-Widget-Platform §4 and
RES-Wallpaper-Platform §3.2's GPU-budget note.

```cpp
struct FrameBudget {
    static constexpr auto TOTAL_FRAME_MS = 16.6; // 60fps target
    static constexpr auto COMPOSITOR_RESERVED_MS = 2.0;  // compositor's own overhead
    static constexpr auto PER_WIDGET_DEFAULT_MS = 2.0;
};

class RenderScheduler {
public:
    // Called once per frame before compositing; returns which layers to
    // render at full quality vs. throttle vs. skip this frame.
    FrameRenderPlan PlanFrame(const std::vector<Layer>& layers, const std::vector<TimingHistory>& history);

    void RecordActualRenderTime(LayerId id, double ms);
private:
    std::unordered_map<LayerId, ThrottleState> _throttleState;
};

enum class ThrottleState { Normal, Throttled /* half framerate */, Suspended /* skipped entirely */ };
```

**Enforcement algorithm (per widget/layer, evaluated every frame):**
1. Track a rolling average of `RecordActualRenderTime` over the last N frames (e.g. N=30).
2. If rolling average exceeds `PER_WIDGET_DEFAULT_MS` for M consecutive frames (e.g. M=60, ~1 second sustained) → transition to `Throttled` (render at half the compositor's frame rate).
3. If still over budget after a further sustained period while `Throttled` → transition to `Suspended`, and signal the Widget Platform via IPC (`WidgetExceededBudget` message, extend ADR-002's proto schema) so the Shell can notify the user and offer to disable the widget — the *decision* to disable lives in the Shell (policy), the *measurement and throttling* lives in the Core (mechanism). This mirrors the sandbox's separation of concerns in ADR-003.
4. Recovery: a `Suspended` or `Throttled` layer is periodically given a trial frame at full budget (e.g. every 10 seconds) to detect if the underlying cause has resolved (e.g., a widget's expensive operation completed) — do not require a full widget restart to recover from transient throttling.

**Compositor's own reserved budget** (`COMPOSITOR_RESERVED_MS`) is not
negotiable and is subtracted from the total before any layer budget is
computed — the compositor itself must never be the thing that causes a missed
frame, since it's shared infrastructure, not a single tenant.

------------------------------------------------------------------------

## 4. Animation Engine

Shared timing/easing primitives used by widget transitions, wallpaper
crossfades, and WindowFX overlay animations (RES-WindowFX §4.3) — one engine, not three.

```cpp
enum class EasingFunction { Linear, EaseInQuad, EaseOutQuad, EaseInOutCubic, /* ... */ };

class Animation {
public:
    Animation(double durationMs, EasingFunction easing);
    double Evaluate(double elapsedMs) const; // returns 0.0-1.0 progress
    bool IsComplete(double elapsedMs) const;
};

class AnimationEngine {
public:
    AnimationHandle Start(Animation anim, std::function<void(double progress)> onTick);
    void Cancel(AnimationHandle handle);
    void Tick(double frameTimeMs); // called once per frame, advances all active animations
};
```

**Design constraint:** the Animation Engine's `Tick` is called from the same
per-frame loop as the Compositor's `RenderFrame` — animations are not a
separate timer/thread, they're driven by the same frame clock, which
guarantees animation smoothness is subject to the same frame-budget
enforcement as everything else (an animation cannot cause a frame to run long; it's evaluated within the existing budget).

------------------------------------------------------------------------

## 5. D3D12 Pipeline Notes

- Single command queue, single swap chain targeting the WorkerW-hosted render
  target window (RES-Desktop-Platform §2).
- Triple-buffering (3 frames in flight) to smooth over minor frame-time
  variance without adding excessive input-to-photon latency — validate this
  choice against measured latency in the Phase 0 spike (RES-Phase-0-1-Implementation
  Step 4) before assuming it's correct; double-buffering is the fallback if
  triple-buffering's extra latency proves noticeable for interactive widget dragging.
- Widget/wallpaper textures arrive via shared memory handles (RES-Widget-Platform
  §4) — imported into the Core's D3D12 device via `ID3D12Device::OpenSharedHandle`,
  not copied through CPU memory.
- Descriptor heap sizing: pre-allocate for a generous fixed widget-count
  ceiling (e.g. 64) rather than dynamically resizing every time a widget is
  added/removed — resizing descriptor heaps mid-session is expensive and
  avoidable; 64 concurrent widgets covers realistic usage with margin.

------------------------------------------------------------------------

## 6. Buffer & Resource Lifecycle

| Resource | Owned by | Released when |
|---|---|---|
| Widget/wallpaper shared texture | Widget/Wallpaper process (source), imported (not owned) by Compositor | Source process unloads/crashes → Compositor detects handle invalidation, removes layer |
| WindowFX overlay resources | Compositor | Target window closes (detected via §3 of RES-WindowFX's window tracking, forwarded over IPC) |
| Animation handles | AnimationEngine | Animation completes or explicitly cancelled by the owning module |

**Leak-prevention discipline:** every `SubmitLayer` must have a corresponding
guaranteed `RemoveLayer` path, including on abnormal termination (crash) of the
layer's source — this is directly testable (RES-Testing-Security-Performance
§A.2 leak-detection loop test referenced in RES-Wallpaper-Platform §8) and
should be treated as a Definition-of-Done item for any new layer-producing feature.

------------------------------------------------------------------------

## 7. Error Handling

| Failure | Handling |
|---|---|
| `OpenSharedHandle` fails (source process died mid-handoff) | Skip that layer for the current frame, do not crash the compositor, retry on next `RegisterWidgetSurface` message |
| GPU device removed/reset (driver crash, external event) | Full device recreation path (`ID3D12Device` re-init, re-import all still-valid shared handles) — this must be a tested, not theoretical, recovery path; GPU resets happen in the real world (driver updates, sleep/resume edge cases) |
| Frame budget exceeded system-wide (not just one layer — e.g. 4K video decode + many widgets) | Throttle lowest-priority layers first (widgets before wallpaper, since wallpaper is the "always visible" baseline) rather than dropping frames uniformly |
| Descriptor heap exhausted (more widgets than the pre-allocated ceiling) | Reject new widget layer registration with a clear IPC error back to the Shell, surfaced to the user as "widget limit reached" rather than crashing |

------------------------------------------------------------------------

## 8. Testing Requirements

- Unit tests: easing function correctness (known input/output pairs), frame budget throttle-state transition logic (mocked timing history, verify Normal→Throttled→Suspended→recovery transitions fire at the correct thresholds).
- Integration tests: real D3D12 device + swap chain on a GPU-capable CI runner (shared requirement with RES-Wallpaper-Platform §7 and RES-WindowFX §8 — this is worth solving once, centrally, in CI infrastructure rather than per-module).
- Stress test: submit the adversarial "misbehaving widget" fixtures (RES-Testing-Security-Performance §A.4) specifically to validate the render scheduler throttles/suspends them without affecting other layers' frame times — this is the test that proves Vision §5's core promise, not just a nice-to-have.
- Leak test: repeated widget load/unload cycles (100+ iterations) with GPU memory usage tracked, must return to baseline — catches shared-handle or descriptor-heap leaks that unit tests can't see.
- Device-loss recovery test: force a simulated device removal (D3D12 debug layer supports this) and verify full recovery without requiring a REX restart.
