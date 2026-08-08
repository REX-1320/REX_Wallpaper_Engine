#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

/* Win11 22H2+ backdrop API (same values ExplorerBlurMica uses) */
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2       /* Mica */
#define DWMSBT_TRANSIENTWINDOW 3  /* Acrylic */
#define DWMSBT_TABBEDWINDOW 4

/* Win10 / pre-22H2 composition API */
#define WCA_ACCENT_POLICY 19
#define ACCENT_DISABLED 0
#define ACCENT_ENABLE_BLURBEHIND 3
#define ACCENT_ENABLE_ACRYLICBLURBEHIND 4

typedef struct _ACCENT_POLICY { DWORD AccentState; DWORD AccentFlags; DWORD GradientColor; DWORD AnimationId; } ACCENT_POLICY;
typedef struct _WCA_DATA { DWORD Attribute; PVOID Data; SIZE_T SizeOfData; } WCA_DATA;
typedef BOOL(WINAPI* PFN_SETWCA)(HWND, WCA_DATA*);
typedef LONG(WINAPI* PFN_DWMSETWA)(HWND, DWORD, LPCVOID, DWORD);
typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

static DWORD buildNumber(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionPtr p = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
    if (!p) return 0;
    RTL_OSVERSIONINFOW vi;
    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (p(&vi) != 0) return 0;
    return vi.dwBuildNumber;
}

static int applyBackdrop(HWND hwnd, int type, int dark) {
    PFN_DWMSETWA fn = (PFN_DWMSETWA)GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute");
    if (!fn) return 0;
    if (dark) {
        BOOL useDark = TRUE;
        fn(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    }
    return fn(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type)) == S_OK;
}

static int applyAccent(HWND hwnd, int state, DWORD tint) {
    PFN_SETWCA fn = (PFN_SETWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return 0;
    ACCENT_POLICY policy;
    policy.AccentState = (DWORD)state;
    policy.AccentFlags = 0;
    policy.GradientColor = tint;
    policy.AnimationId = 0;
    WCA_DATA data;
    data.Attribute = WCA_ACCENT_POLICY;
    data.Data = &policy;
    data.SizeOfData = sizeof(policy);
    return fn(hwnd, &data);
}

typedef struct _FindCtx {
    const wchar_t* title; /* substring match */
    const wchar_t* cls;   /* class name match */
    DWORD pid;
    HWND result;
} FindCtx;

static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    FindCtx* ctx = (FindCtx*)lp;
    if (ctx->pid) {
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != ctx->pid) return TRUE;
    }
    if (ctx->title) {
        wchar_t buf[512];
        GetWindowTextW(hwnd, buf, 512);
        if (!wcsstr(buf, ctx->title)) return TRUE;
    }
    if (ctx->cls) {
        wchar_t buf[256];
        GetClassNameW(hwnd, buf, 256);
        if (wcscmp(buf, ctx->cls) != 0) return TRUE;
    }
    ctx->result = hwnd;
    return FALSE;
}

static void usage(void) {
    printf(
        "mica-cli - apply Mica/acrylic blur to any native window\n\n"
        "Usage: mica-cli --mode <mica|acrylic|blur|remove> [options]\n\n"
        "Window selection (default: foreground window):\n"
        "  --title \"substring\"   match window title\n"
        "  --class \"ClassName\"   match window class\n"
        "  --pid <number>         match process id\n\n"
        "Options:\n"
        "  --dark                 enable dark mode (mica/acrylic)\n"
        "  --tint RRGGBB[AA]      tint color (acrylic, hex)\n"
        "  --help                 show this help\n");
}

static int hexTint(const wchar_t* s, DWORD* out) {
    size_t len = wcslen(s);
    if (len != 6 && len != 8) return 0;
    unsigned long v = wcstoul(s, NULL, 16);
    if (len == 6) v |= 0xFF000000u; /* opaque */
    /* GradientColor is AABBGGRR little-endian */
    *out = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v & 0xFF0000) >> 16) | (v & 0xFF000000);
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 1; }

    const wchar_t* mode = NULL;
    FindCtx ctx = { NULL, NULL, 0, NULL };
    int dark = 0;
    DWORD tint = 0xE6202020;
    int tintSet = 0;

    for (int i = 1; i < argc; i++) {
        const wchar_t* a = argv[i];
        const wchar_t* v = (i + 1 < argc) ? argv[++i] : L"";
        if (wcscmp(a, L"--mode") == 0) mode = v;
        else if (wcscmp(a, L"--title") == 0) ctx.title = v;
        else if (wcscmp(a, L"--class") == 0) ctx.cls = v;
        else if (wcscmp(a, L"--pid") == 0) ctx.pid = (DWORD)_wtoi(v);
        else if (wcscmp(a, L"--dark") == 0) { dark = 1; i--; }
        else if (wcscmp(a, L"--tint") == 0) {
            if (!hexTint(v, &tint)) { printf("bad --tint\n"); return 1; }
            tintSet = 1;
        } else if (wcscmp(a, L"--help") == 0) { usage(); return 0; }
        else { usage(); return 1; }
    }

    HWND hwnd = NULL;
    if (!ctx.title && !ctx.cls && ctx.pid == 0) {
        hwnd = GetForegroundWindow();
    } else {
        EnumWindows(enumProc, (LPARAM)&ctx);
        hwnd = ctx.result;
    }
    if (!hwnd) { printf("no window matched\n"); return 1; }

    wchar_t wbuf[256] = L"(untitled)";
    GetWindowTextW(hwnd, wbuf, 256);
    printf("target: [%ls]\n", wbuf);

    const DWORD build = buildNumber();
    const int newApi = build >= 22621;
    int ok = 0;

    if (mode && wcscmp(mode, L"mica") == 0) {
        ok = newApi ? applyBackdrop(hwnd, DWMSBT_MAINWINDOW, dark)
                    : applyAccent(hwnd, tintSet ? ACCENT_ENABLE_ACRYLICBLURBEHIND : ACCENT_ENABLE_BLURBEHIND, tint);
    } else if (mode && wcscmp(mode, L"acrylic") == 0) {
        ok = newApi ? applyBackdrop(hwnd, DWMSBT_TRANSIENTWINDOW, dark)
                    : applyAccent(hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND, tint);
    } else if (mode && wcscmp(mode, L"blur") == 0) {
        ok = newApi ? applyBackdrop(hwnd, DWMSBT_TRANSIENTWINDOW, dark)
                    : applyAccent(hwnd, ACCENT_ENABLE_BLURBEHIND, 0);
    } else if (mode && wcscmp(mode, L"remove") == 0) {
        ok = newApi ? applyBackdrop(hwnd, DWMSBT_NONE, dark)
                    : applyAccent(hwnd, ACCENT_DISABLED, 0);
    } else {
        usage();
        return 1;
    }

    printf("%s (build %lu, %s)\n", ok ? "applied" : "failed", build, newApi ? "backdrop API" : "composition API");
    return ok ? 0 : 1;
}
