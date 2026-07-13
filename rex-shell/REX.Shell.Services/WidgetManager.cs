using System.Collections.Concurrent;
using System.IO.Compression;

namespace REX.Shell.Services;

public class WidgetManager : IWidgetHostService, IDisposable
{
    private const int MaxCrashRestarts = 3;
    private static readonly TimeSpan CrashWindowMinutes = TimeSpan.FromMinutes(5);

    private readonly string _widgetsDir;
    private readonly string _widgetHostExe;
    private readonly ManifestValidator _validator = new();
    private readonly IpcClient _ipcClient;
    private readonly ConcurrentDictionary<Guid, WidgetInstance> _instances = new();
    private readonly ConcurrentDictionary<Guid, WidgetProcess> _processes = new();
    private readonly ConcurrentDictionary<Guid, List<DateTime>> _crashTimestamps = new();
    private readonly Random _randomOrder = new();

    public WidgetManager(IpcClient ipcClient, string? widgetsDir = null, string? widgetHostExe = null)
    {
        _ipcClient = ipcClient;
        _widgetsDir = widgetsDir ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "REXTools", "widgets");

        _widgetHostExe = widgetHostExe ?? "rex-widget-host.exe";

        Directory.CreateDirectory(_widgetsDir);
    }

    public string ExtractPackage(string packagePath)
    {
        var tempDir = Path.Combine(_widgetsDir, Path.GetFileNameWithoutExtension(packagePath));
        if (Directory.Exists(tempDir))
            Directory.Delete(tempDir, recursive: true);

        ZipFile.ExtractToDirectory(packagePath, tempDir);
        return tempDir;
    }

    public async Task<WidgetInstance> LoadWidgetAsync(string packageId, string profileId)
    {
        var widgetDir = Path.Combine(_widgetsDir, packageId);
        var manifestPath = Path.Combine(widgetDir, "manifest.json");

        if (!Directory.Exists(widgetDir))
            throw new FileNotFoundException($"Widget package not found: {packageId}", widgetDir);
        if (!File.Exists(manifestPath))
            throw new FileNotFoundException($"manifest.json not found in {widgetDir}", manifestPath);

        var manifestJson = await File.ReadAllTextAsync(manifestPath);
        var manifest = _validator.Validate(manifestJson, manifestPath);

        var instance = new WidgetInstance
        {
            InstanceId = Guid.NewGuid(),
            PackageId = packageId,
            Width = manifest.DefaultSize.Width,
            Height = manifest.DefaultSize.Height,
            X = _randomOrder.Next(50, 400),
            Y = _randomOrder.Next(50, 300),
            ZOrder = _instances.Count,
            State = WidgetLifecycleState.Loading
        };

        _instances[instance.InstanceId] = instance;

        var instanceId = instance.InstanceId;
        var process = new WidgetProcess(instanceId);
        process.OnConnected += () => OnWidgetConnected(process, instance);
        process.OnManifestReceived += (json) => { /* verified on load */ };
        process.OnDisconnected += () => HandleCrash(instanceId);
        process.OnError += (_) => HandleCrash(instanceId);
        process.OnSurfaceCreated += (handle, w, h) => OnWidgetSurfaceCreated(instanceId, handle, w, h);

        _processes[instanceId] = process;

        try
        {
            await process.StartAsync(_widgetHostExe, widgetDir);
        }
        catch (Exception ex)
        {
            UpdateState(instanceId, WidgetLifecycleState.Crashed);
            _processes.TryRemove(instanceId, out _);
            throw new InvalidOperationException($"Failed to start widget process for {packageId}", ex);
        }

        return instance;
    }

    public Task UnloadWidgetAsync(Guid instanceId)
    {
        _ipcClient.SendUnregisterWidgetSurface(LayerIdFromGuid(instanceId));

        if (_processes.TryRemove(instanceId, out var process))
        {
            process.Dispose();
        }

        if (_instances.TryRemove(instanceId, out var instance))
        {
            instance.State = WidgetLifecycleState.Stopped;
        }

        return Task.CompletedTask;
    }

    public Task<IReadOnlyList<WidgetInstance>> GetActiveWidgetsAsync()
    {
        var list = _instances.Values
            .Where(i => i.State == WidgetLifecycleState.Running)
            .ToList();

        return Task.FromResult<IReadOnlyList<WidgetInstance>>(list);
    }

    public WidgetInstance? GetInstance(Guid instanceId)
    {
        _instances.TryGetValue(instanceId, out var instance);
        return instance;
    }

    public void UpdatePosition(Guid instanceId, double x, double y, double width, double height)
    {
        if (_instances.TryGetValue(instanceId, out var instance))
        {
            instance.X = x;
            instance.Y = y;
            instance.Width = width;
            instance.Height = height;

            _ipcClient.SendSetWidgetTransform(
                LayerIdFromGuid(instanceId),
                (int)x, (int)y, (int)width, (int)height,
                instance.ZOrder);
        }
    }

    public void ToggleLock(Guid instanceId)
    {
        if (_instances.TryGetValue(instanceId, out var instance))
            instance.Locked = !instance.Locked;
    }

    public void TogglePin(Guid instanceId)
    {
        if (_instances.TryGetValue(instanceId, out var instance))
            instance.Pinned = !instance.Pinned;
    }

    private void UpdateState(Guid instanceId, WidgetLifecycleState state)
    {
        if (_instances.TryGetValue(instanceId, out var instance))
            instance.State = state;
    }

    private void HandleCrash(Guid instanceId)
    {
        UpdateState(instanceId, WidgetLifecycleState.Crashed);

        var now = DateTime.UtcNow;
        var timestamps = _crashTimestamps.GetOrAdd(instanceId, _ => new List<DateTime>());
        timestamps.Add(now);
        timestamps.RemoveAll(t => now - t > CrashWindowMinutes);

        if (timestamps.Count <= MaxCrashRestarts)
        {
            // Auto-restart
            if (_instances.TryGetValue(instanceId, out var instance))
            {
                _ = ReloadAsync(instance.PackageId, instanceId);
            }
        }
    }

    private async Task ReloadAsync(string packageId, Guid oldInstanceId)
    {
        await UnloadWidgetAsync(oldInstanceId);
        try
        {
            await LoadWidgetAsync(packageId, "default");
        }
        catch
        {
            // give up — widget disabled
        }
    }

    private static ulong LayerIdFromGuid(Guid guid)
    {
        var bytes = guid.ToByteArray();
        return BitConverter.ToUInt64(bytes, 0);
    }

    private void OnWidgetConnected(WidgetProcess process, WidgetInstance instance)
    {
        UpdateState(instance.InstanceId, WidgetLifecycleState.Running);
        process.SendCreateSurface((int)instance.Width, (int)instance.Height);
    }

    private void OnWidgetSurfaceCreated(Guid instanceId, ulong handle, int width, int height)
    {
        var layerId = LayerIdFromGuid(instanceId);
        _ipcClient.SendRegisterWidgetSurface(layerId, handle, width, height);

        if (_instances.TryGetValue(instanceId, out var instance))
        {
            _ipcClient.SendSetWidgetTransform(
                layerId, (int)instance.X, (int)instance.Y,
                (int)instance.Width, (int)instance.Height,
                instance.ZOrder);
        }
    }

    public void Dispose()
    {
        foreach (var process in _processes.Values)
            process.Dispose();
        _processes.Clear();
        _instances.Clear();
        GC.SuppressFinalize(this);
    }
}
