# RES-Automation-Engine v1.0

> Depends on: ADR-002, ADR-004, RES-Widget-Platform §5 (SDK surface extension), RES-Desktop-Platform §4 (profiles), RES-Wallpaper-Platform, RES-WindowFX
> Owns: workflow engine, event bus, trigger/condition/action execution, scheduling, history/logging, visual flow editor data contract

------------------------------------------------------------------------

## 1. Scope

The Automation Engine is the connective tissue tying every other module
together: trigger → condition(s) → action(s), where triggers/conditions/actions
can originate from or target any subsystem (widgets, wallpaper, window rules,
profiles, system events). This is deliberately the *last* major module built
(Phase 4) because it depends on stable event surfaces from everything else.

------------------------------------------------------------------------

## 2. Core Concepts & Data Model

### 2.1 Flow

```json
{
  "schemaVersion": 1,
  "flowId": "focus-mode",
  "name": "Focus Mode",
  "enabled": true,
  "trigger": { "type": "schedule", "cron": "0 9 * * MON-FRI" },
  "conditions": [
    { "type": "profileActive", "operator": "notEquals", "value": "meeting-mode" }
  ],
  "actions": [
    { "type": "switchProfile", "params": { "profileId": "work-mode" } },
    { "type": "notify", "params": { "message": "Focus mode enabled" } }
  ]
}
```

### 2.2 Runtime Model

```csharp
public record AutomationFlow
{
    public string FlowId { get; init; }
    public string Name { get; init; }
    public bool Enabled { get; init; }
    public TriggerDefinition Trigger { get; init; }
    public IReadOnlyList<ConditionDefinition> Conditions { get; init; }
    public IReadOnlyList<ActionDefinition> Actions { get; init; }
}

public interface IAutomationEngine
{
    Task<IReadOnlyList<AutomationFlow>> GetFlowsAsync();
    Task SaveFlowAsync(AutomationFlow flow);
    Task DeleteFlowAsync(string flowId);
    Task<FlowExecutionResult> ExecuteManuallyAsync(string flowId); // for testing/manual trigger from UI
}
```

------------------------------------------------------------------------

## 3. Event Bus Architecture

The event bus (`IEventBus`, RES-API-Reference §2) is an in-process pub/sub
mediator in the Shell. Every subsystem publishes lifecycle/state-change events
to it; the Automation Engine is simply the most prominent *subscriber*, not a
privileged owner of the bus — widgets and window rules could theoretically
subscribe to the same events for their own purposes, though only automation
triggers do so in Phase 4 scope.

```csharp
public sealed class EventBus : IEventBus
{
    private readonly ConcurrentDictionary<Type, List<Delegate>> _handlers = new();

    public void Publish<T>(T evt) where T : class
    {
        if (_handlers.TryGetValue(typeof(T), out var handlers))
            foreach (var h in handlers.ToArray())
                ((Action<T>)h).Invoke(evt); // synchronous dispatch; handlers must not block
    }

    public IDisposable Subscribe<T>(Action<T> handler) where T : class { /* ... */ }
}
```

**Design constraint:** dispatch is synchronous and handlers must return
quickly (no I/O inline) — a slow subscriber must not stall event delivery to
other subscribers. Long-running action execution (§5) is dispatched to a
background queue, not run inline inside the event handler.

**Event sources feeding the bus:**
- Core → Shell over IPC (ADR-002): frame stats, crash notifications, widget-surface-ready events.
- Widget Platform: `WidgetCrashed`, `WidgetLoaded`, and widget-originated custom events via `window.rex.automation.trigger()` (RES-API-Reference §3).
- Desktop Platform: `ProfileSwitched`, `MonitorTopologyChanged`.
- OS-level system events (battery, network, session lock/unlock) via Win32 API subscriptions in the Shell.

------------------------------------------------------------------------

## 4. Trigger Types (Phase 4 initial set)

```csharp
public abstract record TriggerDefinition { public abstract string Type { get; } }

public record ScheduleTrigger(string Cron) : TriggerDefinition { public override string Type => "schedule"; }
public record SystemEventTrigger(string EventName) : TriggerDefinition { public override string Type => "systemEvent"; } // e.g. "battery.low", "network.changed", "session.locked"
public record WidgetEventTrigger(string WidgetInstanceId, string EventName) : TriggerDefinition { public override string Type => "widgetEvent"; }
public record ProfileSwitchTrigger(string? FromProfileId, string? ToProfileId) : TriggerDefinition { public override string Type => "profileSwitch"; }
```

Cron parsing: a small, well-tested library (e.g. Cronos for .NET) rather than
hand-rolled — this is exactly the kind of "boring technology" call from Vision
§6's Core Principles addition; scheduling correctness bugs are unpleasant to
debug and not worth reinventing.

------------------------------------------------------------------------

## 5. Action Types & Permission Gating

```csharp
public abstract record ActionDefinition { public abstract string Type { get; } }

public record SwitchProfileAction(string ProfileId) : ActionDefinition { public override string Type => "switchProfile"; }
public record SetWallpaperAction(string MonitorId, string WallpaperAssignmentJson) : ActionDefinition { public override string Type => "setWallpaper"; }
public record NotifyAction(string Message) : ActionDefinition { public override string Type => "notify"; }
public record RunCommandAction(string Command, string[] Args) : ActionDefinition { public override string Type => "runCommand"; }
```

**`RunCommandAction` is a distinct trust tier.** Arbitrary command execution is
the single most dangerous action type in the system — treat it with the same
rigor as RES-SDK-Marketplace §5's permission model:
- Requires explicit, separate user opt-in *per flow* (not a global "allow automation to run commands" toggle) — the consent must be scoped to the specific command being saved, shown at flow-save time.
- Never available to flows imported from a shared/exported profile without re-confirmation — an imported flow with a `runCommand` action must re-prompt on first import, since profile files are meant to be shareable/portable (RES-Desktop-Platform §6) and that portability must not silently import command-execution trust.
- Logged distinctly and prominently in execution history (§7) — this is the action type a user is most likely to want an audit trail for.

------------------------------------------------------------------------

## 6. Visual Flow Editor — Data Contract

The Shell's node-based UI serializes to/from the same `AutomationFlow` JSON
shape (§2.1) — the editor is a view over the data model, not a separate
representation requiring translation. Node graph layout (visual positions of
trigger/condition/action nodes on the canvas) is stored as editor metadata
alongside the flow, not mixed into the execution-relevant schema:

```json
{
  "flowId": "focus-mode",
  "editorMetadata": {
    "nodePositions": { "trigger-1": { "x": 100, "y": 50 }, "action-1": { "x": 100, "y": 200 } }
  }
}
```

This separation means the execution engine never needs to parse UI-layout
data, and the editor never needs a separate persistence path — one document, two consumers.

------------------------------------------------------------------------

## 7. Execution Engine, Scheduling & History

```csharp
public interface IFlowExecutor
{
    Task<FlowExecutionResult> ExecuteAsync(AutomationFlow flow, TriggerContext context);
}

public record FlowExecutionResult(bool Success, IReadOnlyList<ActionResult> ActionResults, TimeSpan Duration);
```

- **Scheduler:** a single background timer service resolves all `ScheduleTrigger` cron expressions and enqueues due flows — one scheduler instance, not one timer per flow, to keep resource usage predictable regardless of flow count.
- **Condition evaluation:** conditions are evaluated synchronously against current state (profile, system info) immediately before action execution — not cached from trigger time, to avoid acting on stale state if there's any delay between trigger and execution.
- **Action execution order:** sequential, not parallel, by default — an action that depends on a prior action's effect (e.g., switch profile, then notify) must not race. Parallel execution is an explicit future opt-in per flow, not the default.
- **History log:** append-only JSON Lines file (`automation-history.jsonl`), one line per flow execution: `{ timestamp, flowId, trigger, conditionResults, actionResults, success }`. Rotated at a size/age threshold (e.g. 10MB or 30 days). Surfaced in a diagnostics view per RES-Testing-Security-Performance's solo-dev debugging rationale — this is how you answer "why didn't my automation fire" without remote access to a user's machine.

------------------------------------------------------------------------

## 8. Error Handling

| Failure | Handling |
|---|---|
| Trigger fires but a condition evaluation throws (e.g., referenced profile no longer exists) | Log as a failed execution in history, do not execute any actions, surface a diagnostic — fail closed, not open |
| An action in a multi-action flow fails partway | Log which actions succeeded/failed individually in `ActionResults`; do not roll back prior successful actions (most REX actions aren't transactional/reversible — e.g., a notification already shown can't be un-shown) — document this non-transactional behavior clearly in user-facing flow editor UI copy |
| `RunCommandAction` command not found / fails to launch | Log full error, never silently swallow — command failures are exactly the class of bug a user needs visibility into |
| Cron expression invalid | Reject at flow-save time with a clear validation error, never silently fail to schedule |
| Event bus handler throws during synchronous dispatch | Catch and log per-handler, do not let one failing subscriber prevent event delivery to others |

------------------------------------------------------------------------

## 9. Testing Requirements

- Unit tests: cron trigger resolution against known time windows, condition evaluation logic per condition type, action serialization round-trip.
- Integration tests: end-to-end flow execution against real (test-instance) profile/wallpaper/window-rule services — the Phase 4 exit criteria from the Vision doc ("battery low → change wallpaper → notify widget works end-to-end") is exactly this test, written once as an automated integration test, not just manually verified once.
- Adversarial test: a flow with a `runCommand` action imported from a profile file, verify the re-confirmation gate (§5) actually blocks silent execution.
- History log test: verify rotation triggers correctly at the size/age threshold and no execution records are lost during rotation.
