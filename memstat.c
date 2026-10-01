// MemStat - tiny Win32 memory widget: system RAM + per-GPU dedicated/shared memory.
// Built without the C runtime; see build.cmd.
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <pdh.h>

#define MAX_GPUS   8
#define ID_TOPMOST 1
#define ID_STARTUP 2
#define ID_EXIT    3
#define ID_ALPHA   10    // ID_ALPHA + i selects kTransparency[i]

static const wchar_t kRunKey[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kAppKey[]  = L"Software\\MemStat";
static const int     kTransparency[] = { 0, 10, 20, 30, 40, 50, 60 };   // percent
static const wchar_t *kTransparencyLabel[] = { L"Off", L"10%", L"20%", L"30%", L"40%", L"50%", L"60%" };
static const GUID kIID_IDXGIFactory1 =
    {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

typedef struct {
    wchar_t  name[128];
    wchar_t  prefix[40];   // "luid_0xHHHHHHHH_0xLLLLLLLL_" perf counter instance prefix
    ULONGLONG total;       // dedicated VRAM
    ULONGLONG dedicated, shared;
} Gpu;

static Gpu       g_gpu[MAX_GPUS];
static int       g_gpuCount;
static ULONGLONG g_ramUsed, g_ramTotal;
static PDH_HQUERY   g_query;
static PDH_HCOUNTER g_cDedicated, g_cShared;
static BYTE      g_pdhBuf[32768];
static UINT      g_dpi = 96;
static HFONT     g_fHeader, g_fTitle, g_fDetail;
static BOOL      g_menuOpen;
static BOOL      g_topmost = TRUE;
static DWORD     g_transparency = 20;   // percent
static int       g_width = 284;         // window width at 96 DPI

// ---- minimal CRT replacements ----
void *memset(void *d, int c, size_t n) { BYTE *p = d; while (n--) *p++ = (BYTE)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; return d; }

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---- data ----
static void InitGpus(void) {
    IDXGIFactory1 *f;
    if (FAILED(CreateDXGIFactory1(&kIID_IDXGIFactory1, (void **)&f))) return;
    IDXGIAdapter1 *a;
    for (UINT i = 0; g_gpuCount < MAX_GPUS && IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            wchar_t prefix[40];
            wsprintfW(prefix, L"luid_0x%08X_0x%08X_", (UINT)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
            BOOL dup = FALSE;
            for (int j = 0; j < g_gpuCount; j++) dup |= lstrcmpW(g_gpu[j].prefix, prefix) == 0;
            if (!dup) {
                Gpu *g = &g_gpu[g_gpuCount++];
                lstrcpynW(g->name, d.Description, 128);
                lstrcpyW(g->prefix, prefix);
                g->total = d.DedicatedVideoMemory;
            }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);

    if (PdhOpenQueryW(NULL, 0, &g_query) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cDedicated);
        PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &g_cShared);
    }
}

static void SumCounter(PDH_HCOUNTER c, int shared) {
    DWORD size = sizeof(g_pdhBuf), count = 0;
    if (!c || PdhGetRawCounterArrayW(c, &size, &count, (PPDH_RAW_COUNTER_ITEM_W)g_pdhBuf) != ERROR_SUCCESS) return;
    PPDH_RAW_COUNTER_ITEM_W items = (PPDH_RAW_COUNTER_ITEM_W)g_pdhBuf;
    for (DWORD i = 0; i < count; i++)
        for (int j = 0; j < g_gpuCount; j++) {
            int n = lstrlenW(g_gpu[j].prefix);
            if (lstrlenW(items[i].szName) >= n &&
                CompareStringOrdinal(items[i].szName, n, g_gpu[j].prefix, n, TRUE) == CSTR_EQUAL) {
                if (shared) g_gpu[j].shared += items[i].RawValue.FirstValue;
                else        g_gpu[j].dedicated += items[i].RawValue.FirstValue;
            }
        }
}

static void Sample(void) {
    MEMORYSTATUSEX m = { sizeof m };
    if (GlobalMemoryStatusEx(&m)) { g_ramTotal = m.ullTotalPhys; g_ramUsed = m.ullTotalPhys - m.ullAvailPhys; }
    for (int j = 0; j < g_gpuCount; j++) g_gpu[j].dedicated = g_gpu[j].shared = 0;
    if (g_query && PdhCollectQueryData(g_query) == ERROR_SUCCESS) {
        SumCounter(g_cDedicated, 0);
        SumCounter(g_cShared, 1);
    }
}

// ---- formatting ----
static wchar_t *Gb(wchar_t *out, ULONGLONG bytes) {
    ULONGLONG t = (bytes * 10 + (1ULL << 29)) >> 30;   // tenths of a GiB, rounded
    wsprintfW(out, L"%u.%u", (UINT)(t / 10), (UINT)(t % 10));
    return out;
}

static UINT Pct(ULONGLONG used, ULONGLONG total) {
    if (!total) return 0;
    ULONGLONG p = (used * 100 + total / 2) / total;
    return p > 100 ? 100 : (UINT)p;
}

// ---- layout / drawing ----
enum { W = 284, MIN_W = 200, MAX_W = 1200, GRIP = 6, PAD = 16, TOP = 12, HEADER = 18, BLOCK = 60, BOTTOM = 8 };

static int WindowHeight(void) { return S(TOP + HEADER + (1 + g_gpuCount) * BLOCK + BOTTOM); }

static HFONT MakeFont(int px, int weight) {
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                       L"Segoe UI Variable Text");
}

static void MakeFonts(void) {
    if (g_fHeader) { DeleteObject(g_fHeader); DeleteObject(g_fTitle); DeleteObject(g_fDetail); }
    g_fHeader = MakeFont(11, FW_SEMIBOLD);
    g_fTitle  = MakeFont(13, FW_SEMIBOLD);
    g_fDetail = MakeFont(11, FW_NORMAL);
}

static void Text(HDC dc, HFONT f, COLORREF c, const wchar_t *s, int x, int y, int w, UINT flags) {
    RECT r = { x, y, x + w, y + S(20) };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | flags);
}

static void Bar(HDC dc, int x, int y, int w, UINT pct, COLORREF c) {
    int h = S(6);
    HBRUSH track = CreateSolidBrush(RGB(76, 76, 76)), fill = CreateSolidBrush(c);
    SelectObject(dc, GetStockObject(NULL_PEN));
    SelectObject(dc, track);
    RoundRect(dc, x, y, x + w + 1, y + h + 1, h, h);
    int fw = w * (int)pct / 100;
    if (fw > 0) {
        if (fw < h) fw = h;
        SelectObject(dc, fill);
        RoundRect(dc, x, y, x + fw + 1, y + h + 1, h, h);
    }
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    DeleteObject(track); DeleteObject(fill);
}

static void Meter(HDC dc, int y, const wchar_t *title, UINT pct, const wchar_t *detail) {
    COLORREF c = pct >= 90 ? RGB(255, 95, 95) : pct >= 75 ? RGB(255, 185, 0) : RGB(76, 194, 255);
    int x = S(PAD), w = S(g_width) - 2 * S(PAD);
    wchar_t p[8];
    wsprintfW(p, L"%u%%", pct);
    Text(dc, g_fTitle, RGB(255, 255, 255), title, x, y + S(6), w - S(50), DT_LEFT);
    Text(dc, g_fTitle, c, p, x, y + S(6), w, DT_RIGHT);
    Bar(dc, x, y + S(29), w, pct, c);
    Text(dc, g_fDetail, RGB(188, 188, 188), detail, x, y + S(39), w, DT_LEFT);
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g_fHeader);

    HBRUSH bg = CreateSolidBrush(RGB(31, 31, 31));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    Text(dc, g_fHeader, RGB(165, 165, 165), L"MEMORY", S(PAD), S(TOP), S(g_width), DT_LEFT);

    wchar_t a[16], b[16], c[16], detail[96];
    int y = S(TOP + HEADER);
    wsprintfW(detail, L"%s / %s GB", Gb(a, g_ramUsed), Gb(b, g_ramTotal));
    Meter(dc, y, L"System RAM", Pct(g_ramUsed, g_ramTotal), detail);
    for (int j = 0; j < g_gpuCount; j++) {
        Gpu *g = &g_gpu[j];
        y += S(BLOCK);
        wsprintfW(detail, L"%s / %s GB dedicated  ·  %s GB shared",
                  Gb(a, g->dedicated), Gb(b, g->total), Gb(c, g->shared));
        Meter(dc, y, g->name, Pct(g->dedicated, g->total), detail);
    }

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// Width (at 96 DPI) needed to show every label without ellipsis. Measures the worst case
// (usage at its maximum) so the window does not need to grow again as the numbers change.
static int TextWidth(HDC dc, HFONT f, const wchar_t *s) {
    SIZE sz;
    SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    return sz.cx;
}

static int FitWidth(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(dc, g_fTitle);
    wchar_t a[16], b[16], c[16], detail[96];
    int need = TextWidth(dc, g_fHeader, L"MEMORY");
    int t = TextWidth(dc, g_fTitle, L"System RAM") + S(50);   // title column is w - S(50) wide
    if (t > need) need = t;
    wsprintfW(detail, L"%s / %s GB", Gb(a, g_ramTotal), Gb(b, g_ramTotal));
    if ((t = TextWidth(dc, g_fDetail, detail)) > need) need = t;
    for (int j = 0; j < g_gpuCount; j++) {
        Gpu *g = &g_gpu[j];
        ULONGLONG sharedMax = g->shared > g_ramTotal / 2 ? g->shared : g_ramTotal / 2;   // default shared limit
        if ((t = TextWidth(dc, g_fTitle, g->name) + S(50)) > need) need = t;
        wsprintfW(detail, L"%s / %s GB dedicated  ·  %s GB shared", Gb(a, g->total), Gb(b, g->total), Gb(c, sharedMax));
        if ((t = TextWidth(dc, g_fDetail, detail)) > need) need = t;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);
    return MulDiv(need + 2 * S(PAD) + S(4), 96, g_dpi);
}

// ---- settings (HKCU\Software\MemStat) ----
static DWORD RegDword(const wchar_t *name, DWORD def) {
    DWORD v, sz = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, kAppKey, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS ? v : def;
}

static void SaveSettings(HWND hwnd) {
    RECT r; GetWindowRect(hwnd, &r);
    DWORD x = (DWORD)r.left, y = (DWORD)r.top, t = g_topmost, a = g_transparency, w = (DWORD)g_width;
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"X", REG_DWORD, &x, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"Y", REG_DWORD, &y, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"Topmost", REG_DWORD, &t, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"Transparency", REG_DWORD, &a, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"Width", REG_DWORD, &w, 4);
}

static BOOL StartupEnabled(void) {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"MemStat", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void SetStartup(BOOL on) {
    if (on) {
        wchar_t path[MAX_PATH + 2] = L"\"";
        GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        lstrcatW(path, L"\"");
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"MemStat", REG_SZ, path, (lstrlenW(path) + 1) * sizeof(wchar_t));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"MemStat");
    }
}

// ---- window ----
static void ApplyTransparency(HWND hwnd) {
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 - 255 * g_transparency / 100), LWA_ALPHA);
}

static void ShowMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), ID_TOPMOST, L"Always on top");
    HMENU sub = CreatePopupMenu();
    for (int i = 0; i < (int)(sizeof kTransparency / sizeof *kTransparency); i++)
        AppendMenuW(sub, MF_STRING | (g_transparency == (DWORD)kTransparency[i] ? MF_CHECKED : 0),
                    ID_ALPHA + i, kTransparencyLabel[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Transparency");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    g_menuOpen = TRUE;
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    g_menuOpen = FALSE;
    DestroyMenu(m);
    switch (cmd) {
    case ID_TOPMOST:
        g_topmost = !g_topmost;
        SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SaveSettings(hwnd);
        break;
    case ID_STARTUP: SetStartup(!StartupEnabled()); break;
    case ID_EXIT:    DestroyWindow(hwnd); break;
    default:
        if (cmd >= ID_ALPHA && cmd < ID_ALPHA + (int)(sizeof kTransparency / sizeof *kTransparency)) {
            g_transparency = kTransparency[cmd - ID_ALPHA];
            ApplyTransparency(hwnd);
            SaveSettings(hwnd);
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        Sample();
        if (g_topmost && !g_menuOpen)  // re-raising while the popup menu is open would cover it
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:      Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    case WM_EXITSIZEMOVE: SaveSettings(hwnd); return 0;
    case WM_NCCALCSIZE:   // no visible frame: WS_THICKFRAME is only there to enable edge resizing
        if (wp) return 0;
        break;
    case WM_NCHITTEST: {
        RECT r; GetWindowRect(hwnd, &r);
        int x = (short)LOWORD(lp);
        if (x < r.left + S(GRIP))  return HTLEFT;
        if (x >= r.right - S(GRIP)) return HTRIGHT;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {   // width is resizable; height always fits the content
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(MIN_W);
        mm->ptMaxTrackSize.x = S(MAX_W);
        mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = WindowHeight();
        return 0;
    }
    case WM_SIZE:
        if (LOWORD(lp) >= S(MIN_W)) g_width = MulDiv(LOWORD(lp), 96, g_dpi);   // skip the 1x1 creation size
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_RBUTTONUP:    ShowMenu(hwnd); return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g_dpi = HIWORD(wp);
        MakeFonts();
        SetWindowPos(hwnd, NULL, r->left, r->top, S(g_width), WindowHeight(), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY:
        SaveSettings(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void WinMainCRTStartup(void) {
    // Single instance.
    CreateMutexW(NULL, TRUE, L"MemStatWidget");
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE hi = GetModuleHandleW(NULL);

    WNDCLASSW wc = {0};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"MemStat";
    RegisterClassW(&wc);

    InitGpus();
    Sample();

    // Place at saved position (if still on a monitor), else top-right of the primary work area.
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT pt = { (LONG)RegDword(L"X", 0x80000000), (LONG)RegDword(L"Y", 0x80000000) };
    BOOL saved = pt.x != (LONG)0x80000000 && MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!saved) { pt.x = wa.right - 1; pt.y = wa.top; }

    g_topmost = RegDword(L"Topmost", 1);
    g_transparency = RegDword(L"Transparency", 20);
    if (g_transparency > 90) g_transparency = 20;
    g_width = (int)RegDword(L"Width", W);
    if (g_width < MIN_W || g_width > MAX_W) g_width = W;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | (g_topmost ? WS_EX_TOPMOST : 0), L"MemStat", L"MemStat",
                                WS_POPUP | WS_THICKFRAME, pt.x, pt.y, 1, 1, NULL, NULL, hi, NULL);
    ApplyTransparency(hwnd);
    g_dpi = GetDpiForWindow(hwnd);
    MakeFonts();
    int fit = FitWidth(hwnd);   // grow (never shrink) the saved width so no text is cut off
    if (g_width < fit) g_width = fit > MAX_W ? MAX_W : fit;
    if (!saved) { pt.x = wa.right - S(g_width) - S(16); pt.y = wa.top + S(16); }
    SetWindowPos(hwnd, NULL, pt.x, pt.y, S(g_width), WindowHeight(), SWP_NOZORDER | SWP_NOACTIVATE);
    DWORD corner = 2 /* DWMWCP_ROUND */, dark = TRUE;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 1000, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess(0);
}
