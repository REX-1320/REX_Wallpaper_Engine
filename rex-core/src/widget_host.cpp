#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl.h>
#include <WebView2.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <shellapi.h>

using Microsoft::WRL::ComPtr;

// --- COM smart pointer helpers (lightweight, avoid WRL dependency for callbacks) ---
template <typename T>
class ComHolder {
    T* ptr_ = nullptr;
public:
    ComHolder() = default;
    ~ComHolder() { if (ptr_) ptr_->Release(); }
    ComHolder(const ComHolder&) = delete;
    ComHolder& operator=(const ComHolder&) = delete;
    void reset(T* p = nullptr) { if (ptr_) ptr_->Release(); ptr_ = p; }
    T* get() const { return ptr_; }
    T** address() { return &ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
};

static constexpr DWORD PIPE_BUFFER_SIZE = 4096;
static constexpr UINT RENDER_TIMER_ID = 1;
static constexpr UINT RENDER_INTERVAL_MS = 33; // ~30fps

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static volatile bool g_running = true;
static std::string g_manifestJson;
static std::string g_manifestDir;

// --- Hidden window for WebView2 ---
static HWND g_hiddenWnd = nullptr;
static const wchar_t* HIDDEN_WND_CLASS = L"REXWidgetHostHidden";

// --- D3D12 Shared Texture ---
static ComPtr<ID3D12Device> g_d3dDevice;
static ComPtr<ID3D12CommandQueue> g_d3dQueue;
static ComPtr<ID3D12CommandAllocator> g_cmdAlloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmdList;
static ComPtr<ID3D12Fence> g_d3dFence;
static UINT64 g_fenceValue = 0;
static HANDLE g_fenceEvent = nullptr;

struct WidgetTexture {
    ComPtr<ID3D12Resource> resource;
    HANDLE sharedHandle = nullptr;
    int width = 0;
    int height = 0;
};
static WidgetTexture g_widgetTexture;

// --- WebView2 ---
static ComPtr<ICoreWebView2> g_webview2;
static ComPtr<ICoreWebView2Controller> g_webview2Controller;
static bool g_webview2Ready = false;
static bool g_contentDirty = true;
static bool g_captureInProgress = false;

// --- WIC factory (lazy init) ---
static ComPtr<IWICImagingFactory> g_wicFactory;

static bool EnsureWicFactory() {
    if (!g_wicFactory) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wicFactory)))) {
            return false;
        }
    }
    return true;
}

// --- D3D12 init (with fence) ---
static bool InitD3D12() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        printf("[WidgetHost] Failed to create DXGI factory\n");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&g_d3dDevice)))) {
            break;
        }
    }
    if (!g_d3dDevice) {
        printf("[WidgetHost] Failed to create D3D12 device\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(g_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_d3dQueue)))) {
        printf("[WidgetHost] Failed to create command queue\n");
        return false;
    }

    if (FAILED(g_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&g_cmdAlloc)))) {
        printf("[WidgetHost] Failed to create command allocator\n");
        return false;
    }

    if (FAILED(g_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&g_cmdList)))) {
        printf("[WidgetHost] Failed to create command list\n");
        return false;
    }
    g_cmdList->Close();

    if (FAILED(g_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3dFence)))) {
        printf("[WidgetHost] Failed to create fence\n");
        return false;
    }

    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        printf("[WidgetHost] Failed to create fence event\n");
        return false;
    }

    printf("[WidgetHost] D3D12 initialized\n");
    return true;
}

static void GpuFlush() {
    if (!g_d3dQueue || !g_d3dFence) return;

    g_fenceValue++;
    g_d3dQueue->Signal(g_d3dFence.Get(), g_fenceValue);
    if (g_d3dFence->GetCompletedValue() < g_fenceValue) {
        g_d3dFence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

static bool CreateSharedTexture(int width, int height) {
    if (!g_d3dDevice) return false;

    g_widgetTexture.resource.Reset();
    if (g_widgetTexture.sharedHandle) {
        CloseHandle(g_widgetTexture.sharedHandle);
        g_widgetTexture.sharedHandle = nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = static_cast<UINT64>(width);
    texDesc.Height = static_cast<UINT>(height);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 0.0f;

    ComPtr<ID3D12Resource> resource;
    if (FAILED(g_d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
        IID_PPV_ARGS(&resource)))) {
        printf("[WidgetHost] Failed to create texture (%dx%d)\n", width, height);
        return false;
    }

    HANDLE sharedHandle = nullptr;
    if (FAILED(g_d3dDevice->CreateSharedHandle(resource.Get(), nullptr,
        GENERIC_ALL, nullptr, &sharedHandle))) {
        printf("[WidgetHost] Failed to create shared handle\n");
        return false;
    }

    g_widgetTexture.resource = std::move(resource);
    g_widgetTexture.sharedHandle = sharedHandle;
    g_widgetTexture.width = width;
    g_widgetTexture.height = height;

    printf("[WidgetHost] Created shared texture (%dx%d) handle=0x%p\n",
        width, height, sharedHandle);
    return true;
}

// --- Decode PNG/JPEG via WIC and upload to shared texture ---
static bool DecodeAndUpload(IStream* stream, int expectedW, int expectedH) {
    if (!EnsureWicFactory()) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(g_wicFactory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);

    // Convert to 32bpp BGRA
    ComPtr<IWICBitmapSource> converted;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &converted))) {
        return false;
    }

    // Use actual captured size if it matches, otherwise skip
    if (static_cast<int>(w) != expectedW || static_cast<int>(h) != expectedH) {
        printf("[WidgetHost] WebView2 capture size mismatch: got %dx%d, expected %dx%d\n",
            w, h, expectedW, expectedH);
        return false;
    }

    // Read pixels
    std::vector<uint8_t> pixels(w * h * 4);
    WICRect rect = { 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
    if (FAILED(converted->CopyPixels(&rect, w * 4, w * h * 4, pixels.data()))) {
        return false;
    }

    // Upload to D3D12 shared texture
    if (!g_d3dDevice || !g_widgetTexture.resource) return false;

    UINT rowPitch = w * 4;
    UINT64 uploadSize = rowPitch * h;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> uploadBuffer;
    if (FAILED(g_d3dDevice->CreateCommittedResource(&uploadHeapProps,
        D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuffer)))) {
        return false;
    }

    void* mapped = nullptr;
    if (FAILED(uploadBuffer->Map(0, nullptr, &mapped))) return false;
    memcpy(mapped, pixels.data(), uploadSize);
    uploadBuffer->Unmap(0, nullptr);

    // Transition texture to copy destination
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_widgetTexture.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    g_cmdAlloc->Reset();
    g_cmdList->Reset(g_cmdAlloc.Get(), nullptr);
    g_cmdList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Width = w;
    srcLoc.PlacedFootprint.Footprint.Height = h;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = g_widgetTexture.resource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    g_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition back to render target
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdList->ResourceBarrier(1, &barrier);

    g_cmdList->Close();
    ID3D12CommandList* lists[] = { g_cmdList.Get() };
    g_d3dQueue->ExecuteCommandLists(1, lists);

    GpuFlush();

    return true;
}

// --- Forward declare window proc ---
static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- WebView2 Controller Created Handler (defined first since it's needed by EnvironmentCreatedHandler) ---
class ControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    LONG refCount_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&refCount_);
        if (ref == 0) delete this;
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller* controller) override {
        if (FAILED(errorCode) || !controller) {
            printf("[WidgetHost] WebView2 controller creation failed: 0x%08x\n", errorCode);
            return S_OK;
        }

        g_webview2Controller = controller;
        ICoreWebView2* webview2 = nullptr;
        controller->get_CoreWebView2(&webview2);
        if (!webview2) {
            printf("[WidgetHost] Failed to get CoreWebView2 from controller\n");
            return S_OK;
        }
        g_webview2 = webview2;

        // Enable scripting
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(g_webview2->get_Settings(&settings))) {
            settings->put_IsScriptEnabled(TRUE);
        }

        // Set up navigation completed event
        webview2->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    g_contentDirty = true;
                    printf("[WidgetHost] WebView2 navigation completed\n");
                    return S_OK;
                }
            ).Get(), nullptr);

        // Show controller and resize
        controller->put_IsVisible(TRUE);
        RECT bounds;
        bounds.left = 0;
        bounds.top = 0;
        bounds.right = g_widgetTexture.width;
        bounds.bottom = g_widgetTexture.height;
        controller->put_Bounds(bounds);

        // Navigate to widget HTML
        std::string htmlPath = g_manifestDir + "\\index.html";
        int len = MultiByteToWideChar(CP_UTF8, 0, htmlPath.c_str(), -1, nullptr, 0);
        std::wstring wHtmlPath(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, htmlPath.c_str(), -1, wHtmlPath.data(), len);
        g_webview2->Navigate(wHtmlPath.c_str());

        g_webview2Ready = true;
        printf("[WidgetHost] WebView2 initialized, navigating to %s\n", htmlPath.c_str());
        return S_OK;
    }
};

// --- WebView2 Environment Created Handler ---
class EnvironmentCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    LONG refCount_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&refCount_);
        if (ref == 0) delete this;
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment* env) override {
        if (FAILED(errorCode) || !env) {
            printf("[WidgetHost] WebView2 environment creation failed: 0x%08x\n", errorCode);
            return S_OK;
        }

        // Create controller with async handler
        auto* ctrlHandler = new ControllerCreatedHandler();
        env->CreateCoreWebView2Controller(g_hiddenWnd, ctrlHandler);
        ctrlHandler->Release();
        return S_OK;
    }
};

// --- WebView2 Capture Preview Handler ---
class CaptureCompletedHandler : public ICoreWebView2CapturePreviewCompletedHandler {
    LONG refCount_ = 1;
    IStream* stream_;
    int width_;
    int height_;
public:
    CaptureCompletedHandler(IStream* stream, int w, int h)
        : stream_(stream), width_(w), height_(h) {
        stream_->AddRef();
    }
    ~CaptureCompletedHandler() { stream_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2CapturePreviewCompletedHandler)) {
            *ppv = static_cast<ICoreWebView2CapturePreviewCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&refCount_);
        if (ref == 0) delete this;
        return ref;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode) override {
        g_captureInProgress = false;
        if (FAILED(errorCode)) {
            printf("[WidgetHost] CapturePreview failed: 0x%08x\n", errorCode);
            return S_OK;
        }

        // Rewind stream and decode
        LARGE_INTEGER zero = {};
        stream_->Seek(zero, STREAM_SEEK_SET, nullptr);
        DecodeAndUpload(stream_, width_, height_);
        return S_OK;
    }
};

// --- Hidden window proc ---
static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == RENDER_TIMER_ID) {
        if (g_webview2Ready && g_contentDirty && g_widgetTexture.resource && !g_captureInProgress) {
            g_contentDirty = false;
            g_captureInProgress = true;

            IStream* stream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
                auto* handler = new CaptureCompletedHandler(stream,
                    g_widgetTexture.width, g_widgetTexture.height);
                g_webview2->CapturePreview(
                    COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                    stream, handler);
                stream->Release();
            } else {
                g_captureInProgress = false;
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- Simple IPC (non-blocking with overlapped I/O) ---
static bool ConnectToPipe(const std::string& pipeName) {
    for (int attempt = 0; attempt < 5; attempt++) {
        g_pipe = CreateFileA(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr
        );
        if (g_pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(g_pipe, &mode, nullptr, nullptr);
            return true;
        }
        Sleep(1000 * (attempt + 1));
    }
    return false;
}

static bool SendRaw(uint32_t type, const void* data, DWORD dataSize) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;

    char header[5] = {};
    memcpy(header + 1, &type, sizeof(type));

    std::vector<char> buffer(5 + dataSize);
    memcpy(buffer.data(), header, 5);
    if (data && dataSize > 0)
        memcpy(buffer.data() + 5, data, dataSize);

    DWORD written = 0;
    return WriteFile(g_pipe, buffer.data(), buffer.size(), &written, nullptr) != FALSE;
}

// Non-blocking poll - returns true if a message was processed
static bool TryReadMessage() {
    DWORD bytesAvailable = 0;
    if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            g_running = false;
        }
        return false;
    }

    if (bytesAvailable < 5) return false;

    char header[5];
    DWORD bytesRead = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = ReadFile(g_pipe, header, 5, &bytesRead, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 0);
        DWORD transferred = 0;
        ok = GetOverlappedResult(g_pipe, &ov, &transferred, FALSE);
        bytesRead = transferred;
    }
    CloseHandle(ov.hEvent);

    if (!ok || bytesRead != 5) return false;

    uint32_t type;
    memcpy(&type, header + 1, sizeof(type));

    if (type == 99) {
        g_running = false;
        return true;
    }

    char temp[PIPE_BUFFER_SIZE];
    DWORD payloadBytes = 0;
    OVERLAPPED ov2 = {};
    ov2.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    ok = ReadFile(g_pipe, temp, PIPE_BUFFER_SIZE, &payloadBytes, &ov2);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov2.hEvent, 0);
        DWORD transferred = 0;
        ok = GetOverlappedResult(g_pipe, &ov2, &transferred, FALSE);
        payloadBytes = transferred;
    }
    CloseHandle(ov2.hEvent);

    switch (type) {
    case 1: { // ManifestRequest
        SendRaw(2, g_manifestJson.data(), g_manifestJson.size());
        printf("[WidgetHost] Manifest sent\n");
        break;
    }
    case 3: { // Ping -> Pong
        SendRaw(4, "pong", 4);
        break;
    }
    case 10: { // CreateSurface
        if (payloadBytes >= 8) {
            int32_t w, h;
            memcpy(&w, temp, sizeof(w));
            memcpy(&h, temp + 4, sizeof(h));
            if (CreateSharedTexture(w, h)) {
                char payload[16] = {};
                uint64_t handleVal = reinterpret_cast<uintptr_t>(g_widgetTexture.sharedHandle);
                memcpy(payload, &handleVal, sizeof(handleVal));
                memcpy(payload + 8, &w, sizeof(w));
                memcpy(payload + 12, &h, sizeof(h));
                SendRaw(11, payload, sizeof(payload));
                printf("[WidgetHost] SurfaceCreated sent (%dx%d handle=0x%llx)\n",
                    w, h, handleVal);

                // Initialize WebView2 with this texture size
                if (g_hiddenWnd && !g_webview2Ready) {
                    CreateCoreWebView2EnvironmentWithOptions(
                        nullptr, nullptr, nullptr,
                        new EnvironmentCreatedHandler());
                } else if (g_webview2Ready && g_webview2Controller) {
                    RECT bounds;
                    bounds.left = 0;
                    bounds.top = 0;
                    bounds.right = w;
                    bounds.bottom = h;
                    g_webview2Controller->put_Bounds(bounds);
                }
            } else {
                printf("[WidgetHost] CreateSurface failed (%dx%d)\n", w, h);
            }
        }
        break;
    }
    }
    return true;
}

// --- Manifest Loading ---
static bool LoadManifest(const std::string& manifestDir) {
    std::string manifestPath = manifestDir + "\\manifest.json";
    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        printf("[WidgetHost] ERROR: Cannot open %s\n", manifestPath.c_str());
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    g_manifestJson = ss.str();
    return !g_manifestJson.empty();
}

// --- Register hidden window class ---
static bool RegisterHiddenWindowClass() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = HIDDEN_WND_CLASS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static bool CreateHiddenWindow() {
    g_hiddenWnd = CreateWindowExW(
        0, HIDDEN_WND_CLASS, L"REX Widget Host",
        WS_POPUP,
        0, 0, 640, 480,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );
    if (!g_hiddenWnd) {
        printf("[WidgetHost] Failed to create hidden window\n");
        return false;
    }
    return true;
}

// --- Main ---
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    std::string pipeName;

    int argc;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, arg.data(), len, nullptr, nullptr);

        if (arg == "--pipe" && i + 1 < argc) {
            i++;
            len = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
            pipeName.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, pipeName.data(), len, nullptr, nullptr);
        } else if (arg == "--manifest-dir" && i + 1 < argc) {
            i++;
            len = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
            g_manifestDir.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, g_manifestDir.data(), len, nullptr, nullptr);
        }
    }
    LocalFree(argvW);

    if (pipeName.empty() || g_manifestDir.empty()) {
        printf("[WidgetHost] Usage: rex-widget-host --pipe <pipeName> --manifest-dir <dir>\n");
        return 1;
    }

    printf("[WidgetHost] Starting (PID: %lu, pipe: %s, manifest: %s)\n",
        GetCurrentProcessId(), pipeName.c_str(), g_manifestDir.c_str());

    if (!LoadManifest(g_manifestDir)) {
        printf("[WidgetHost] ERROR: Failed to load manifest\n");
        return 1;
    }

    // Initialize COM (required for WebView2 and WIC)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!InitD3D12()) {
        printf("[WidgetHost] WARNING: D3D12 init failed (rendering unavailable)\n");
    }

    RegisterHiddenWindowClass();

    if (!CreateHiddenWindow()) {
        printf("[WidgetHost] ERROR: Failed to create hidden window\n");
        CoUninitialize();
        return 1;
    }

    if (!ConnectToPipe(pipeName)) {
        printf("[WidgetHost] ERROR: Failed to connect to Shell pipe\n");
        CoUninitialize();
        return 1;
    }

    printf("[WidgetHost] Connected to Shell. Starting message pump...\n");

    // Set render timer
    SetTimer(g_hiddenWnd, RENDER_TIMER_ID, RENDER_INTERVAL_MS, nullptr);

    // Message pump (handles both WebView2 messages and IPC polling)
    while (g_running) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }

        TryReadMessage();

        Sleep(1); // prevent CPU spinning
    }

    // Cleanup
    if (g_webview2Controller) {
        g_webview2Controller->Close();
        g_webview2Controller.Reset();
    }
    g_webview2.Reset();

    if (g_hiddenWnd) {
        DestroyWindow(g_hiddenWnd);
        g_hiddenWnd = nullptr;
    }

    g_widgetTexture.resource.Reset();
    if (g_widgetTexture.sharedHandle) {
        CloseHandle(g_widgetTexture.sharedHandle);
        g_widgetTexture.sharedHandle = nullptr;
    }
    g_d3dFence.Reset();
    g_cmdList.Reset();
    g_cmdAlloc.Reset();
    g_d3dQueue.Reset();
    g_d3dDevice.Reset();

    if (g_fenceEvent) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }

    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }

    g_wicFactory.Reset();
    CoUninitialize();

    printf("[WidgetHost] Exiting\n");
    return 0;
}
