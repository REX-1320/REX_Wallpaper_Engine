namespace REX.Shell.Services;

public record WidgetManifest
{
    public int SchemaVersion { get; init; }
    public string Id { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string Version { get; init; } = string.Empty;
    public string Author { get; init; } = string.Empty;
    public string Entry { get; init; } = string.Empty;
    public WidgetSize DefaultSize { get; init; } = new();
    public WidgetSize? MinSize { get; init; }
    public WidgetPermissions Permissions { get; init; } = new();
    public List<string> DataBindings { get; init; } = [];
}

public record WidgetSize
{
    public int Width { get; init; }
    public int Height { get; init; }
}

public record WidgetPermissions
{
    public bool Network { get; init; }
    public List<string> DataSources { get; init; } = [];
    public bool Storage { get; init; }
}

public record WidgetInstance
{
    public Guid InstanceId { get; init; }
    public string PackageId { get; init; } = string.Empty;
    public double X { get; set; }
    public double Y { get; set; }
    public double Width { get; set; }
    public double Height { get; set; }
    public int ZOrder { get; set; }
    public bool Locked { get; set; }
    public bool Pinned { get; set; }
    public string? GroupId { get; set; }
    public WidgetLifecycleState State { get; set; }
}

public enum WidgetLifecycleState
{
    Stopped, Loading, Running, Suspended, Crashed
}

public interface IDataSource
{
    string Key { get; }
    TimeSpan PollInterval { get; }
    Task<DataSourceValue> ReadAsync();
    IObservable<DataSourceValue> Changes { get; }
}

public record DataSourceValue
{
    public string Key { get; init; } = string.Empty;
    public object? Value { get; init; }
    public long Timestamp { get; init; }
    public string? Error { get; init; }
}

public interface IWidgetHostService
{
    Task<WidgetInstance> LoadWidgetAsync(string packageId, string profileId);
    Task UnloadWidgetAsync(Guid instanceId);
    Task<IReadOnlyList<WidgetInstance>> GetActiveWidgetsAsync();
}

public interface IWidgetLayoutRepository
{
    Task<IReadOnlyList<WidgetInstance>> GetLayoutAsync(string profileId);
    Task SaveLayoutAsync(string profileId, IReadOnlyList<WidgetInstance> layout);
}

public record MonitorInfo
{
    public string DeviceId { get; init; } = string.Empty;
    public int X { get; init; }
    public int Y { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public bool IsPrimary { get; init; }
    public float DpiScale { get; init; }
}

public interface IMonitorTopologyService
{
    IReadOnlyList<MonitorInfo> GetMonitors();
    event EventHandler? TopologyChanged;
}

public record Profile
{
    public string ProfileId { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public Dictionary<string, string> Monitors { get; init; } = [];
    public string? WindowRulesRef { get; init; }
    public List<string> AutomationFlowsActive { get; init; } = [];
}

public interface IProfileService
{
    Task<Profile> GetActiveProfileAsync();
    Task SwitchProfileAsync(string profileId);
    Task<Profile> SaveAsNewProfileAsync(string name);
    Task DeleteProfileAsync(string profileId);
}

public interface IDesktopHost
{
    Task<bool> InitializeAsync();
    bool IsReady { get; }
    event EventHandler? HostLost;
}

public interface IEventBus
{
    void Publish<T>(T evt) where T : class;
    IDisposable Subscribe<T>(Action<T> handler) where T : class;
}

public interface IWallpaperService
{
    Task AssignAsync(string monitorId, WallpaperAssignment assignment);
    Task<WallpaperAssignment?> GetAssignmentAsync(string monitorId);
    Task ClearAsync(string monitorId);
}

public record WallpaperAssignment
{
    public string MonitorId { get; init; } = string.Empty;
    public WallpaperType Type { get; init; }
    public string SourcePath { get; init; } = string.Empty;
    public bool AudioReactive { get; init; }
    public bool Loop { get; init; }
    public bool Muted { get; init; }
    public FillMode FillMode { get; init; }
}

public enum WallpaperType { Static, Video, Web, Interactive }
public enum FillMode { Fill, Fit, Stretch, Tile, Center }
public enum SyncMode { Independent, Spanned, Mirrored }

public interface IAutomationEngine
{
    Task<IReadOnlyList<AutomationFlow>> GetFlowsAsync();
    Task SaveFlowAsync(AutomationFlow flow);
    Task DeleteFlowAsync(string flowId);
    Task<FlowExecutionResult> ExecuteManuallyAsync(string flowId);
}

public record AutomationFlow
{
    public string FlowId { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public bool Enabled { get; init; }
    public TriggerDefinition Trigger { get; init; } = new ScheduleTrigger("");
    public List<ConditionDefinition> Conditions { get; init; } = [];
    public List<ActionDefinition> Actions { get; init; } = [];
}

public abstract record TriggerDefinition { public abstract string Type { get; } }
public record ScheduleTrigger(string Cron) : TriggerDefinition { public override string Type => "schedule"; }
public record SystemEventTrigger(string EventName) : TriggerDefinition { public override string Type => "systemEvent"; }
public record WidgetEventTrigger(string WidgetInstanceId, string EventName) : TriggerDefinition { public override string Type => "widgetEvent"; }
public record ProfileSwitchTrigger(string? FromProfileId, string? ToProfileId) : TriggerDefinition { public override string Type => "profileSwitch"; }

public abstract record ConditionDefinition { public abstract string Type { get; }
    public string Operator { get; init; } = "equals";
    public string Value { get; init; } = string.Empty;
}
public record ProfileActiveCondition(string ProfileId) : ConditionDefinition { public override string Type => "profileActive"; }

public abstract record ActionDefinition { public abstract string Type { get; } }

public record SwitchProfileAction(string ProfileId) : ActionDefinition { public override string Type => "switchProfile"; }
public record SetWallpaperAction(string MonitorId, string WallpaperAssignmentJson) : ActionDefinition { public override string Type => "setWallpaper"; }
public record NotifyAction(string Message) : ActionDefinition { public override string Type => "notify"; }
public record RunCommandAction(string Command, string[] Args) : ActionDefinition { public override string Type => "runCommand"; }

public record FlowExecutionResult(bool Success, IReadOnlyList<ActionResult> ActionResults, TimeSpan Duration);

public record ActionResult
{
    public string ActionType { get; init; } = string.Empty;
    public bool Success { get; init; }
    public string? Error { get; init; }
}
