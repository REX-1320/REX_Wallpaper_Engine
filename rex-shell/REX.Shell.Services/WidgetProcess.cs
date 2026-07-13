using System.Diagnostics;
using System.IO.Pipes;
using System.Text;

namespace REX.Shell.Services;

public class WidgetProcess : IDisposable
{
    private const int HeaderSize = 5;
    private const int BufferSize = 4096;
    private const int MaxReconnectAttempts = 5;

    private readonly Guid _instanceId;
    private readonly string _pipeName;
    private NamedPipeServerStream? _pipe;
    private Process? _process;
    private CancellationTokenSource? _cts;
    private bool _disposed;

    public event Action? OnConnected;
    public event Action<string>? OnManifestReceived;
    public event Action? OnDisconnected;
    public event Action<Exception>? OnError;
    public event Action<ulong, int, int>? OnSurfaceCreated;

    public Guid InstanceId => _instanceId;
    public bool IsConnected { get; private set; }
    public int ProcessId => _process?.Id ?? 0;

    public WidgetProcess(Guid instanceId)
    {
        _instanceId = instanceId;
        _pipeName = $@"\.\pipe\REX\widget\{instanceId:N}\v1";
    }

    public async Task StartAsync(string widgetExePath, string manifestDir)
    {
        _cts = new CancellationTokenSource();

        _pipe = new NamedPipeServerStream(_pipeName, PipeDirection.InOut,
            NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Message, PipeOptions.Asynchronous);

        var psi = new ProcessStartInfo
        {
            FileName = widgetExePath,
            Arguments = $"--pipe \"{_pipeName}\" --manifest-dir \"{manifestDir}\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };

        _process = new Process { StartInfo = psi, EnableRaisingEvents = true };
        _process.Exited += (_, _) =>
        {
            if (!_disposed) OnDisconnected?.Invoke();
        };

        _process.Start();

        await Task.Run(() => _pipe.WaitForConnection(), _cts.Token);
        IsConnected = true;
        OnConnected?.Invoke();

        _ = Task.Run(() => ReadLoopAsync(_cts.Token));
        SendRaw(1, []); // ManifestRequest
    }

    private async Task ReadLoopAsync(CancellationToken ct)
    {
        var buffer = new byte[BufferSize];
        while (!ct.IsCancellationRequested)
        {
            try
            {
                int bytesRead = await _pipe!.ReadAsync(buffer, 0, buffer.Length, ct);
                if (bytesRead <= HeaderSize) break;

                uint type = BitConverter.ToUInt32(buffer, 1);

                switch (type)
                {
                    case 2: // ManifestResponse
                        var json = Encoding.UTF8.GetString(buffer, HeaderSize, bytesRead - HeaderSize);
                        OnManifestReceived?.Invoke(json);
                        break;

                    case 11: // SurfaceCreated
                        if (bytesRead >= HeaderSize + 16)
                        {
                            ulong handle = BitConverter.ToUInt64(buffer, HeaderSize);
                            int w = BitConverter.ToInt32(buffer, HeaderSize + 8);
                            int h = BitConverter.ToInt32(buffer, HeaderSize + 12);
                            OnSurfaceCreated?.Invoke(handle, w, h);
                        }
                        break;
                }
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                OnError?.Invoke(ex);
                break;
            }
        }
    }

    public void SendCreateSurface(int width, int height)
    {
        var bytes = new byte[8];
        BitConverter.GetBytes(width).CopyTo(bytes, 0);
        BitConverter.GetBytes(height).CopyTo(bytes, 4);
        SendRaw(10, bytes);
    }

    public void SendPing()
    {
        SendRaw(3, Encoding.UTF8.GetBytes("ping"));
    }

    public void SendExit()
    {
        SendRaw(99, []);
    }

    public void SendRaw(uint type, byte[] payload)
    {
        if (_pipe is null || !IsConnected) return;

        var header = new byte[HeaderSize];
        BitConverter.GetBytes(type).CopyTo(header, 1);

        var data = new byte[HeaderSize + payload.Length];
        header.CopyTo(data, 0);
        payload.CopyTo(data, HeaderSize);

        try
        {
            _pipe.Write(data, 0, data.Length);
            _pipe.Flush();
        }
        catch
        {
            // disconnected
        }
    }

    public void Kill()
    {
        if (_process is not null && !_process.HasExited)
        {
            _process.Kill(entireProcessTree: true);
            _process.WaitForExit(2000);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _cts?.Cancel();
        SendExit();
        Kill();

        _pipe?.Dispose();
        _process?.Dispose();
        _cts?.Dispose();

        IsConnected = false;
        GC.SuppressFinalize(this);
    }
}
