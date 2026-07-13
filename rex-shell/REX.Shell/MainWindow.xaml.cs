using Microsoft.UI.Xaml;
using REX.Shell.Services;

namespace REX.Shell;

public sealed partial class MainWindow : Window
{
    private readonly IpcClient _ipcClient;
    private readonly WidgetManager _widgetManager;

    public MainWindow()
    {
        _ipcClient = new IpcClient();
        _widgetManager = new WidgetManager(_ipcClient);
        InitializeComponent();
        Title = "REX Tools v0.1.0";
        _ipcClient.OnConnected += () =>
            DispatcherQueue.TryEnqueue(() => StatusText.Text = "Connected");
        _ipcClient.OnDisconnected += () =>
            DispatcherQueue.TryEnqueue(() => StatusText.Text = "Disconnected");

        WidgetList.ItemsSource = _widgetManager.GetActiveWidgetsAsync().Result;
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

    private async void OnInstallClick(object sender, RoutedEventArgs e)
    {
        var filePicker = new Windows.Storage.Pickers.FileOpenPicker
        {
            ViewMode = Windows.Storage.Pickers.PickerViewMode.List,
            SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.Downloads
        };
        filePicker.FileTypeFilter.Add(".rexwidget");

        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(filePicker, hwnd);

        var file = await filePicker.PickSingleFileAsync();
        if (file is null) return;

        try
        {
            StatusText.Text = "Installing widget...";

            var packageDir = _widgetManager.ExtractPackage(file.Path);
            var packageId = Path.GetFileName(packageDir);

            var instance = await _widgetManager.LoadWidgetAsync(packageId, "default");
            StatusText.Text = $"Widget loaded: {instance.PackageId} (PID: {_widgetManager.GetActiveWidgetsAsync().Result.Count} active)";

            WidgetList.ItemsSource = null;
            WidgetList.ItemsSource = await _widgetManager.GetActiveWidgetsAsync();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Install failed: {ex.Message}";
        }
    }

    private void OnSizeChanged(object sender, WindowSizeChangedEventArgs args)
    {
    }

    protected override void OnClosed(EventArgs e)
    {
        _widgetManager.Dispose();
        _ipcClient.Disconnect();
        base.OnClosed(e);
    }
}
