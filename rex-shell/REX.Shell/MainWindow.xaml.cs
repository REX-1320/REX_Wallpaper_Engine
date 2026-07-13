using Microsoft.UI.Xaml;
using REX.Shell.Services;

namespace REX.Shell;

public partial class MainWindow : Window
{
    private readonly IpcClient _ipcClient = new();

    public MainWindow()
    {
        InitializeComponent();
        Title = "REX Tools v0.1.0";
        _ipcClient.OnConnected += () =>
            DispatcherQueue.TryEnqueue(() => StatusText.Text = "Connected");
        _ipcClient.OnDisconnected += () =>
            DispatcherQueue.TryEnqueue(() => StatusText.Text = "Disconnected");
    }

    private void OnConnectClick(object sender, RoutedEventArgs e)
    {
        _ = _ipcClient.ConnectAsync();
    }

    private void OnColorClick(object sender, RoutedEventArgs e)
    {
        var random = new Random();
        float r = random.NextSingle();
        float g = random.NextSingle();
        float b = random.NextSingle();
        _ipcClient.SendSetTriangleColor(r, g, b);
    }

    private void OnSizeChanged(object sender, WindowSizeChangedEventArgs args)
    {
    }

    protected override void OnClosed(EventArgs e)
    {
        _ipcClient.Disconnect();
        base.OnClosed(e);
    }
}
