// ─── REX Wallpaper Preview Client ─────────────────────────────────────────
//
// A minimal cross-process preview shell renderer for the wallpaper engine.
//
// Spawns rex-wallpaper-runtime.exe --runtime as a child, binds the client
// PID via SET_CLIENT_PID, and consumes published video frames through the
// shared GPU surfaces:
//
//   INIT / SET_CLIENT_PID / LOAD / GET_FRAME_SURFACE  (JSON over stdin)
//   FRAME_READY events + shared fence + NT-shared slot textures (GPU)
//
// The client is pure D3D11: the pool slots are D3D11 BGRA8 (SDR) or
// RGBA16F (HDR) textures and the fence is a shared D3D12 fence that D3D11
// opens with ID3D11Device5::OpenSharedFence (same pattern the decoder
// uses). Frames are drawn into a plain window as a fullscreen quad.
//
// Usage: rex-preview [--video <path>] [--frames <N>] [--runtime-path <path>]
//   --frames 0 (default) runs until the window is closed or the runtime dies.

#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wallpaper/runtime/Json.hpp"

using Microsoft::WRL::ComPtr;

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

uint64_t ParseHex64(const std::string& s, uint64_t def = 0) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return std::strtoull(s.c_str(), nullptr, 16);
    }
    return def;
}

std::string HexU64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx",
                  static_cast<unsigned long long>(v));
    return buf;
}

// ─── Runtime child process + frame protocol ───────────────────────────────

struct RuntimeProcess {
    PROCESS_INFORMATION pi = {};
    HANDLE hStdin = nullptr;
    HANDLE hStdout = nullptr;
    bool alive = false;

    bool Start(const std::wstring& exePath) {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE stdinR = nullptr;
        if (!CreatePipe(&stdinR, &hStdin, &sa, 0)) return false;
        SetHandleInformation(hStdin, HANDLE_FLAG_INHERIT, 0);  // parent-side end

        HANDLE stdoutW = nullptr;
        if (!CreatePipe(&hStdout, &stdoutW, &sa, 0)) return false;
        SetHandleInformation(hStdout, HANDLE_FLAG_INHERIT, 0);  // parent-side end

        std::wstring cmd = L"\"" + exePath + L"\" --runtime";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdinR;    // child reads from the read end
        si.hStdOutput = stdoutW;  // child writes to the write end
        si.hStdError = stdoutW;   // runtime logs to file; stderr is unused

        BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(stdinR);   // parent's copies of the child-side ends
        CloseHandle(stdoutW);
        if (!ok) return false;
        alive = true;
        return true;
    }

    void SendJson(const rex::runtime::Json& j) {
        std::string payload = j.Dump();
        uint32_t len = static_cast<uint32_t>(payload.size());
        DWORD written = 0;
        WriteFile(hStdin, &len, 4, &written, nullptr);
        WriteFile(hStdin, payload.data(), len, &written, nullptr);
        FlushFileBuffers(hStdin);
    }

    void SendCommand(int id, const std::string& cmd, rex::runtime::Json::Object params) {
        rex::runtime::Json::Object o;
        o.emplace("type", rex::runtime::Json("command"));
        o.emplace("id", rex::runtime::Json(id));
        o.emplace("cmd", rex::runtime::Json(cmd));
        o.emplace("params", rex::runtime::Json(std::move(params)));
        SendJson(rex::runtime::Json(std::move(o)));
    }

    // Reads one length-prefixed JSON frame; false if EOF/error. Caller must
    // have ensured data is available (WAIT_OBJECT_0 on hStdout).
    bool ReadFrame(std::string& out) {
        uint32_t len = 0;
        DWORD n = 0;
        if (!ReadFile(hStdout, &len, 4, &n, nullptr) || n != 4) return false;
        if (len == 0 || len > 16 * 1024 * 1024) return false;
        out.resize(len);
        DWORD total = 0;
        while (total < len) {
            if (!ReadFile(hStdout, &out[total], len - total, &n, nullptr)) return false;
            total += n;
        }
        return true;
    }

    // Non-blocking: true when at least one complete frame is buffered in the
    // pipe. The drain loop must NOT block in ReadFile — a live runtime
    // streams frames continuously, so a blocking read would starve the
    // window message pump (WM_CLOSE/WM_QUIT would never be dispatched).
    bool FrameAvailable() {
        if (!hStdout) return false;
        DWORD avail = 0, total = 0;
        if (!PeekNamedPipe(hStdout, nullptr, 0, nullptr, &avail, &total) ||
            avail < 4) {
            return false;
        }
        uint32_t len = 0;
        DWORD n = 0;
        if (!PeekNamedPipe(hStdout, &len, 4, &n, nullptr, &total) || n != 4) {
            return false;
        }
        return avail >= 4 + len;
    }

    void Stop() {
        if (!alive) return;
        SendCommand(999, "SHUTDOWN", {});
        // Give the runtime a moment to shut down cleanly (it also exits on
        // stdin EOF, which happens when we close the pipe).
        CloseHandle(hStdin);
        hStdin = nullptr;
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE) {
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (hStdout) CloseHandle(hStdout);
        hStdout = nullptr;
        alive = false;
    }

    ~RuntimeProcess() { Stop(); }
};

// ─── D3D11 preview renderer ───────────────────────────────────────────────

struct PreviewRenderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11Fence> fence;
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11PixelShader> psHdr;
    ComPtr<ID3D11SamplerState> sampler;
    std::vector<ComPtr<ID3D11Texture2D>> slots;
    std::vector<ComPtr<ID3D11ShaderResourceView>> slotSrvs;
    uint64_t generation = UINT64_MAX;  // force re-acquire on first frame
    int width = 0, height = 0;
    bool hdrSlots = false;  // slots carry PQ-encoded BT.2020 in RGBA16F
    bool initialized = false;

    bool Init(HWND hwnd, const LUID& adapterLuid) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        ComPtr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

        // Match the runtime's adapter so shared textures open on the same GPU.
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = E_FAIL;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc;
            a->GetDesc1(&desc);
            if (desc.AdapterLuid.HighPart == adapterLuid.HighPart &&
                desc.AdapterLuid.LowPart == adapterLuid.LowPart) {
                adapter = a;
                break;
            }
        }

        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                     : D3D_DRIVER_TYPE_HARDWARE,
                               nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                               &device, nullptr, &ctx);
        if (FAILED(hr) || FAILED(device.As(&device1)) ||
            FAILED(device.As(&device5)) ||
            FAILED(ctx.As(&ctx4))) return false;

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = 1280;
        scd.Height = 720;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.Scaling = DXGI_SCALING_STRETCH;

        hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &scd, nullptr,
                                             nullptr, &swapchain);
        if (FAILED(hr)) return false;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        initialized = CompileShaders();
        return initialized;
    }

    bool CompileShaders() {
        const char* hlsl = R"(
            struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
            VSOut VS(uint id : SV_VertexID) {
                VSOut o;
                o.uv = float2(float((id << 1) & 2), float(id & 2));
                o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
                return o;
            }
            Texture2D tex : register(t0);
            SamplerState smp : register(s0);
            float4 PS(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }
        )";

        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "preview", nullptr, nullptr,
                                "VS", "vs_5_0", 0, 0, &blob, &err);
        if (FAILED(hr) || !blob) {
            std::fprintf(stderr, "VS compile failed: %s\n",
                         err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                   nullptr, &vs);

        hr = D3DCompile(hlsl, strlen(hlsl), "preview", nullptr, nullptr,
                        "PS", "ps_5_0", 0, 0, &blob, &err);
        if (FAILED(hr) || !blob) {
            std::fprintf(stderr, "PS compile failed: %s\n",
                         err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                  nullptr, &ps);

        // HDR slots carry PQ-encoded (ST.2084) BT.2020 values; decode the
        // EOTF, convert primaries, tone map to SDR and re-encode sRGB so the
        // preview matches the wallpaper on an SDR swapchain.
        const char* hlslHdr = R"(
            struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
            Texture2D tex : register(t0);
            SamplerState smp : register(s0);
            float3 PQDecode(float3 e) {
                const float m1 = 2610.0 / 16384.0;
                const float m2 = 2523.0 / 4096.0 * 128.0;
                const float c1 = 3424.0 / 4096.0;
                const float c2 = 2413.0 / 4096.0 * 32.0;
                const float c3 = 2392.0 / 4096.0 * 32.0;
                float3 p = pow(max(e, 0.0), 1.0 / m2);
                return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
            }
            float3 BT2020To709(float3 c) {
                float3 o;
                o.r = 1.66049 * c.r - 0.58764 * c.g - 0.07283 * c.b;
                o.g = -0.12456 * c.r + 1.13290 * c.g - 0.00832 * c.b;
                o.b = -0.01816 * c.r - 0.10057 * c.g + 1.11873 * c.b;
                return o;
            }
            float4 PS(VSOut i) : SV_Target {
                float3 pq = tex.Sample(smp, i.uv).rgb;
                float3 lin = PQDecode(pq) * 10000.0;      // nits
                float3 mapped = lin / (1.0 + lin / 1000.0);  // Reinhard
                float3 s = BT2020To709(mapped / 1000.0);
                s = s <= 0.0031308 ? s * 12.92 : 1.055 * pow(s, 1.0 / 2.4) - 0.055;
                return float4(s, 1.0);
            }
        )";
        hr = D3DCompile(hlslHdr, strlen(hlslHdr), "preview-hdr", nullptr, nullptr,
                        "PS", "ps_5_0", 0, 0, &blob, &err);
        if (FAILED(hr) || !blob) {
            std::fprintf(stderr, "HDR PS compile failed: %s\n",
                         err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                  nullptr, &psHdr);

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, &sampler);
        return vs && ps && sampler;
    }

    // Opens the shared fence + slot textures reported by GET_FRAME_SURFACE.
    bool AcquireSurface(const rex::runtime::Json& data) {
        if (!data["available"].AsBool(false)) return false;

        uint64_t newGen = ParseHex64(data["generation"].AsString());
        HANDLE fenceHandle = reinterpret_cast<HANDLE>(
            ParseHex64(data["fenceHandle"].AsString()));

        if (newGen == generation && fence) return true;

        fence.Reset();
        if (!fenceHandle ||
            FAILED(device5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&fence)))) {
            std::fprintf(stderr, "OpenSharedFence failed\n");
            return false;
        }

        const auto& slotsArr = data["slots"].AsArray();
        slots.clear();
        slotSrvs.clear();
        for (const auto& slot : slotsArr) {
            HANDLE h = reinterpret_cast<HANDLE>(
                ParseHex64(slot["handle"].AsString()));
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11Resource> res;
            if (!h || FAILED(device1->OpenSharedResource1(h, IID_PPV_ARGS(&res)))) {
                std::fprintf(stderr, "OpenSharedHandle failed for slot\n");
                continue;
            }
            res.As(&tex);
            ComPtr<ID3D11ShaderResourceView> srv;
            D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
            D3D11_TEXTURE2D_DESC td = {};
            tex->GetDesc(&td);
            hdrSlots = (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
            std::fprintf(stderr, "slot %d format %s (%dx%d)\n", static_cast<int>(slotSrvs.size()),
                         hdrSlots ? "R16G16B16A16_FLOAT (HDR)" : "B8G8R8A8_UNORM (SDR)",
                         static_cast<int>(td.Width), static_cast<int>(td.Height));
            srvd.Format = td.Format;
            srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(tex.Get(), &srvd, &srv);
            if (srv) {
                slots.push_back(tex);
                slotSrvs.push_back(srv);
            }
        }

        generation = newGen;
        width = static_cast<int>(data["width"].AsNumber(0));
        height = static_cast<int>(data["height"].AsNumber(0));
        return !slots.empty();
    }

    void RenderFrame(UINT slot, uint64_t fenceValue) {
        if (slot >= slotSrvs.size() || !fence) return;

        static int dbg = 0;
        if (dbg < 5) {
            std::fprintf(stderr, "render slot=%u fence=0x%llx\n", slot,
                         static_cast<unsigned long long>(fenceValue));
        }

        ctx4->Wait(fence.Get(), fenceValue);  // GPU-side ordering hint

        // The shared fence is signaled by the runtime after the blt; wait on
        // the CPU so the draw below never races the producer.
        uint64_t completed = 0;
        for (int t = 0; t < 200; ++t) {
            completed = fence->GetCompletedValue();
            if (completed >= fenceValue) break;
            Sleep(1);
        }
        if (completed < fenceValue) {
            std::fprintf(stderr, "fence timeout (have 0x%llx need 0x%llx)\n",
                         static_cast<unsigned long long>(completed),
                         static_cast<unsigned long long>(fenceValue));
        }

        if (dbg < 5) {
            D3D11_TEXTURE2D_DESC rd = {};
            slots[slot]->GetDesc(&rd);
            rd.Usage = D3D11_USAGE_STAGING;
            rd.BindFlags = 0;
            rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            rd.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> rb;
            if (SUCCEEDED(device->CreateTexture2D(&rd, nullptr, &rb))) {
                ctx->CopyResource(rb.Get(), slots[slot].Get());
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(ctx->Map(rb.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                    const uint8_t* p = static_cast<const uint8_t*>(map.pData);
                    unsigned long long minL = 255, maxL = 0, sumL = 0, n = 0;
                    unsigned long long minC = 765, maxC = 0, sumC = 0;
                    for (UINT y = 0; y < rd.Height; y += 16) {
                        for (UINT x = 0; x < rd.Width; x += 16) {
                            const uint8_t* px = p + y * map.RowPitch + x * 4;
                            unsigned v = (px[0] + px[1] + px[2]) / 3;
                            if (v < minL) minL = v;
                            if (v > maxL) maxL = v;
                            sumL += v;
                            unsigned c = (unsigned)(abs((int)px[0] - (int)px[1]) +
                                                     abs((int)px[1] - (int)px[2]) +
                                                     abs((int)px[2] - (int)px[0]));
                            if (c < minC) minC = c;
                            if (c > maxC) maxC = c;
                            sumC += c;
                            n++;
                        }
                    }
                    std::fprintf(stderr,
                                 "  slot[%u] luma min=%llu max=%llu mean=%.1f "
                                 "chroma min=%llu max=%llu mean=%.1f\n",
                                 slot, minL, maxL, (double)sumL / (double)n,
                                 minC, maxC, (double)sumC / (double)n);
                    ctx->Unmap(rb.Get(), 0);
                }
            }
            dbg++;
        }

        swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        D3D11_RENDER_TARGET_VIEW_DESC rtd = {};
        D3D11_TEXTURE2D_DESC td = {};
        backBuffer->GetDesc(&td);
        rtd.Format = td.Format;
        rtd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(backBuffer.Get(), &rtd, &rtv);

        FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(hdrSlots ? psHdr.Get() : ps.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        ID3D11ShaderResourceView* srv = slotSrvs[slot].Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp = { 0, 0, 1280, 720, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
        ctx->Flush();
        swapchain->Present(1, 0);
    }
};

// ─── Window + app state ───────────────────────────────────────────────────

struct AppState {
    HWND hwnd = nullptr;
    HWND btnApply = nullptr;
    HWND btnRemove = nullptr;
    RuntimeProcess runtime;
    PreviewRenderer renderer;
    std::string videoPath;
    int framesTarget = 0;
    int framesShown = 0;
    uint64_t surfaceRequestedAt = 0;
    bool surfaceRequested = false;
    bool quit = false;
    bool initialized = false;
    bool wallpaperApplied = false;
};

enum {
    IDC_APPLY = 1001,
    IDC_REMOVE = 1002,
};

void SetWallpaperUi(AppState& app, bool applied) {
    app.wallpaperApplied = applied;
    if (app.btnApply) EnableWindow(app.btnApply, !applied);
    if (app.btnRemove) EnableWindow(app.btnRemove, applied);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* app = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_APPLY || id == IDC_REMOVE) {
            if (app) {
                rex::runtime::Json::Object p;
                p.emplace("enabled", rex::runtime::Json(id == IDC_APPLY));
                app->runtime.SendCommand(id == IDC_APPLY ? 50 : 51,
                                         "SET_WALLPAPER", std::move(p));
            }
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HandleFrame(AppState& app, const std::string& raw) {
    auto parsed = rex::runtime::Json::Parse(raw);
    if (!parsed) return;
    const rex::runtime::Json& f = *parsed;
    const std::string type = f["type"].AsString();
    if (type == "response") {
        // Responses are fire-and-forget here; errors are printed.
        if (!f["ok"].AsBool(true)) {
            std::fprintf(stderr, "command %d failed: %s\n",
                         f["id"].AsInt(-1), f["error"].AsString("?").c_str());
        }
        app.surfaceRequested = false;
        if (f["id"].AsInt(-1) == 100) {
            // The renderer is created lazily once the runtime reports the
            // surface's adapter (must match the GPU that owns the textures).
            if (!app.renderer.initialized) {
                const uint64_t lu = ParseHex64(f["data"]["adapterLuid"].AsString());
                LUID luid{};
                luid.LowPart = static_cast<DWORD>(lu & 0xFFFFFFFFull);
                luid.HighPart = static_cast<LONG>(lu >> 32);
                if (!app.renderer.Init(app.hwnd, luid)) {
                    std::fprintf(stderr, "renderer init failed\n");
                    app.quit = true;
                    return;
                }
            }
            app.renderer.AcquireSurface(f["data"]);
        } else if (f["id"].AsInt(-1) == 50) {
            SetWallpaperUi(app, true);
        } else if (f["id"].AsInt(-1) == 51) {
            SetWallpaperUi(app, false);
        }
        return;
    }
    if (type == "event" && f["event"].AsString() == "FRAME_READY") {
        const auto& d = f["data"];
        const UINT slot = static_cast<UINT>(d["slot"].AsNumber(0));
        const uint64_t fenceValue = ParseHex64(d["fenceValue"].AsString());
        const int w = static_cast<int>(d["width"].AsNumber(0));
        const int h = static_cast<int>(d["height"].AsNumber(0));

        // Lazy surface acquisition: first frame, size change, or every ~5s
        // (the runtime only re-duplicates handles when the generation
        // changed, so this is cheap).
        if (!app.surfaceRequested &&
            (app.renderer.generation == UINT64_MAX ||
             app.renderer.width != w || app.renderer.height != h ||
             GetTickCount64() - app.surfaceRequestedAt > 5000)) {
            app.surfaceRequested = true;
            app.surfaceRequestedAt = GetTickCount64();
            app.runtime.SendCommand(100, "GET_FRAME_SURFACE", {});
            return;  // draw the frame once the surface response arrives
        }

        app.renderer.RenderFrame(slot, fenceValue);
        app.framesShown++;
        std::wstring title = L"REX Preview - " + Utf8ToWide(app.videoPath) +
                             L" [" + std::to_wstring(w) + L"x" + std::to_wstring(h) +
                             L"] frame " + std::to_wstring(app.framesShown);
        SetWindowTextW(app.hwnd, title.c_str());
        if (app.framesTarget > 0 && app.framesShown >= app.framesTarget) {
            app.quit = true;
        }
    }
}

int Run(const std::wstring& runtimePath, const std::wstring& videoPath,
        int framesTarget) {
    // Window (shown immediately; frames appear when the runtime publishes).
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t CLASS_NAME[] = L"REXPreviewWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"REX Preview - starting...",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 2;
    ShowWindow(hwnd, SW_SHOW);

    AppState app;
    app.hwnd = hwnd;
    app.videoPath = WideToUtf8(videoPath);
    app.framesTarget = framesTarget;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    // Desktop controls (child buttons; the preview itself stays in its own
    // window until "Apply to Desktop" is pressed).
    app.btnApply = CreateWindowExW(0, L"BUTTON", L"Apply to Desktop",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   8, 508, 130, 24,
                                   hwnd, reinterpret_cast<HMENU>(IDC_APPLY),
                                   hInst, nullptr);
    app.btnRemove = CreateWindowExW(0, L"BUTTON", L"Remove Wallpaper",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   146, 508, 130, 24,
                                    hwnd, reinterpret_cast<HMENU>(IDC_REMOVE),
                                    hInst, nullptr);
    SetWallpaperUi(app, false);

    if (!app.runtime.Start(runtimePath)) {
        std::fprintf(stderr, "failed to start runtime: %s\n", WideToUtf8(runtimePath).c_str());
        DestroyWindow(hwnd);
        return 2;
    }

    // Protocol bootstrap.
    rex::runtime::Json::Object initParams;
    initParams.emplace("maxFps", rex::runtime::Json(60));
    initParams.emplace("hardwareAcceleration", rex::runtime::Json(true));
    // Preview-only by default: the desktop is untouched until the user
    // presses "Apply to Desktop" (SET_WALLPAPER).
    initParams.emplace("applyToDesktop", rex::runtime::Json(false));
    app.runtime.SendCommand(1, "INIT", std::move(initParams));

    rex::runtime::Json::Object pidParams;
    pidParams.emplace("pid", rex::runtime::Json(static_cast<int>(GetCurrentProcessId())));
    app.runtime.SendCommand(2, "SET_CLIENT_PID", std::move(pidParams));

    // Wait for INIT to land before LOAD (the runtime processes frames in order,
    // so a couple of ms of pump time below suffices).
    Sleep(50);

    rex::runtime::Json::Object loadParams;
    loadParams.emplace("path", rex::runtime::Json(WideToUtf8(videoPath)));
    loadParams.emplace("type", rex::runtime::Json("video"));
    loadParams.emplace("loop", rex::runtime::Json(true));
    loadParams.emplace("muted", rex::runtime::Json(true));
    app.runtime.SendCommand(3, "LOAD", std::move(loadParams));

    // LOAD stops at scene creation; video starts on the explicit PLAY command.
    app.runtime.SendCommand(4, "PLAY", {});

    // Main loop: pump window messages + consume protocol frames.
    while (!app.quit) {
        DWORD wait = MsgWaitForMultipleObjectsEx(1, &app.runtime.hStdout, 250,
                                                 QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            std::string frame;
            while (app.runtime.FrameAvailable()) {
                if (!app.runtime.ReadFrame(frame)) break;
                HandleFrame(app, frame);
            }
            if (app.runtime.pi.hProcess &&
                WaitForSingleObject(app.runtime.pi.hProcess, 0) == WAIT_OBJECT_0) {
                std::fprintf(stderr, "runtime exited unexpectedly\n");
                break;
            }
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { app.quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyWindow(hwnd);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::wstring videoPath = L"test.mp4";
    std::wstring runtimePath;
    int framesTarget = 0;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--video" && i + 1 < argc) videoPath = argv[++i];
        else if (arg == L"--runtime-path" && i + 1 < argc) runtimePath = argv[++i];
        else if (arg == L"--frames" && i + 1 < argc) framesTarget = _wtoi(argv[++i]);
        else if (arg == L"--help") {
            std::fwprintf(stderr,
                L"REX Preview Client\n"
                L"  --video <path>       video file to preview (default: test.mp4)\n"
                L"  --frames <N>         exit after N frames (0 = run until closed)\n"
                L"  --runtime-path <p>   path to rex-wallpaper-runtime.exe\n");
            return 0;
        }
    }

    if (runtimePath.empty()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t* slash = wcsrchr(self, L'\\');
        if (slash) *slash = L'\0';
        runtimePath = std::wstring(self) + L"\\rex-wallpaper-runtime.exe";
    }

    std::fprintf(stderr, "preview: runtime=%s video=%s frames=%d\n",
                 WideToUtf8(runtimePath).c_str(), WideToUtf8(videoPath).c_str(),
                 framesTarget);
    return Run(runtimePath, videoPath, framesTarget);
}
