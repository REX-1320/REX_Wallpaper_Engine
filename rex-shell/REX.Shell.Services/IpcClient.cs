using System.IO.Pipes;
using System.Text;

namespace REX.Shell.Services;

public class IpcClient : IDisposable
{
    private const string PipeName = @"\\.\pipe\REX\core\v1";
    private const int MaxReconnectAttempts = 5;
    private const int ReconnectBaseDelayMs = 1000;
    private const int PingIntervalMs = 5000;

    private NamedPipeClientStream? _pipe;
    private CancellationTokenSource? _cts;
    private bool _connected;

    public event Action? OnConnected;
    public event Action? OnDisconnected;
    public event Action<string>? OnMessageReceived;

    public bool IsConnected => _connected;

    public async Task ConnectAsync()
    {
        _cts = new CancellationTokenSource();
        for (int attempt = 0; attempt < MaxReconnectAttempts; attempt++)
        {
            try
            {
                _pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut,
                    PipeOptions.Asynchronous);
                await _pipe.ConnectAsync(3000, _cts.Token);
                _connected = true;
                OnConnected?.Invoke();

                _ = Task.Run(() => PingLoop(_cts.Token));
                _ = Task.Run(() => ReadLoop(_cts.Token));
                return;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[REX] Connection attempt {attempt + 1} failed: {ex.Message}");
                await Task.Delay(ReconnectBaseDelayMs * (attempt + 1), _cts.Token);
            }
        }
        Console.WriteLine("[REX] Failed to connect after all attempts");
    }

    public void Disconnect()
    {
        _cts?.Cancel();
        _connected = false;
        _pipe?.Dispose();
        _pipe = null;
        OnDisconnected?.Invoke();
    }

    public void SendPing()
    {
        SendRaw(MessageType.Ping, Encoding.UTF8.GetBytes("ping"));
    }

    public void SendGetStatus()
    {
        SendRaw(MessageType.GetStatus, []);
    }

    public void SendSetTriangleColor(float r, float g, float b)
    {
        var bytes = new byte[12];
        BitConverter.GetBytes(r).CopyTo(bytes, 0);
        BitConverter.GetBytes(g).CopyTo(bytes, 4);
        BitConverter.GetBytes(b).CopyTo(bytes, 8);
        SendRaw(MessageType.SetTriangleColor, bytes);
    }

    private void SendRaw(MessageType type, byte[] payload)
    {
        if (_pipe is null || !_connected) return;

        var header = new byte[5];
        header[0] = 0;
        BitConverter.GetBytes((uint)type).CopyTo(header, 1);

        var data = new byte[header.Length + payload.Length];
        header.CopyTo(data, 0);
        payload.CopyTo(data, header.Length);

        try
        {
            _pipe.Write(data, 0, data.Length);
            _pipe.Flush();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[REX] Send failed: {ex.Message}");
            Disconnect();
        }
    }

    private async Task PingLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && _connected)
        {
            try
            {
                SendPing();
                await Task.Delay(PingIntervalMs, ct);
            }
            catch { break; }
        }
    }

    private async Task ReadLoop(CancellationToken ct)
    {
        var buffer = new byte[4096];
        while (!ct.IsCancellationRequested && _connected)
        {
            try
            {
                int bytesRead = await _pipe!.ReadAsync(buffer, 0, buffer.Length, ct);
                if (bytesRead == 0) break;

                string message = Encoding.UTF8.GetString(buffer, 0, bytesRead);
                OnMessageReceived?.Invoke(message);
            }
            catch { break; }
        }
        if (_connected)
        {
            _connected = false;
            OnDisconnected?.Invoke();
        }
    }

    public void Dispose()
    {
        Disconnect();
        GC.SuppressFinalize(this);
    }

    private enum MessageType : uint
    {
        Ping = 1,
        GetStatus = 4,
        Shutdown = 6,
        SetTriangleColor = 99
    }
}
