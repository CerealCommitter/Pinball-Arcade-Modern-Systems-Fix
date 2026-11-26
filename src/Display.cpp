// Pinball Arcade fix: screen resolution.
//
// The DX11 build creates an ordinary overlapped window from saved
// WindowX/Y/W/H values (default 0,0 with the title bar pushed off-screen, so
// it sits small in the corner and can't be dragged) and sizes its swap chain
// to that window's client area. It has no way to fill a modern monitor while
// keeping the picture's shape.
//
// We intercept the creation of the game window and instead make it a
// borderless window at the largest size of the chosen aspect ratio that fits
// the monitor, centred, with a black backdrop window behind it supplying the
// bars. The game renders natively at that size (e.g. 2560x1440 on a
// 5120x1440 monitor), so nothing is stretched.
#include "Common.h"
#include <stdlib.h>
#include <wchar.h>

// ---------------------------------------------------------------- settings
enum class Mode { Pillarbox, Off };
static Mode g_mode = Mode::Pillarbox;
static int g_aspectW = 16, g_aspectH = 9; // 0:0 = fill the whole monitor

static void LoadDisplaySettings()
{
    wchar_t ini[MAX_PATH];
    IniPath(ini);
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Display", L"Mode", L"Fullscreen", buf, 64, ini);
    g_mode = (_wcsicmp(buf, L"Off") == 0 || _wcsicmp(buf, L"Original") == 0) ? Mode::Off : Mode::Pillarbox;

    GetPrivateProfileStringW(L"Display", L"AspectRatio", L"16:9", buf, 64, ini);
    if (_wcsicmp(buf, L"Native") == 0 || _wcsicmp(buf, L"Fill") == 0) {
        g_aspectW = g_aspectH = 0;
    } else {
        int w = 0, h = 0;
        if (swscanf_s(buf, L"%d:%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_aspectW = w;
            g_aspectH = h;
        }
    }
}

// Largest rect of the configured aspect ratio that fits in `mon`, centred.
static RECT FitRect(const RECT& mon)
{
    const int mw = mon.right - mon.left, mh = mon.bottom - mon.top;
    int w = mw, h = mh;
    if (g_aspectW && g_aspectH) {
        if ((long long)mw * g_aspectH > (long long)mh * g_aspectW)
            w = (int)((long long)mh * g_aspectW / g_aspectH); // monitor wider: bars left/right
        else
            h = (int)((long long)mw * g_aspectH / g_aspectW); // monitor taller: bars top/bottom
    }
    RECT r;
    r.left = mon.left + (mw - w) / 2;
    r.top = mon.top + (mh - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

// ---------------------------------------------------------------- backdrop
static HWND g_backdrop, g_game;
static WNDPROC g_gameProc;

static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE:
        // clicking a bar hands focus to the game instead of the backdrop
        if (g_game) SetForegroundWindow(g_game);
        return MA_NOACTIVATE;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND CreateBackdrop(const RECT& mon, HINSTANCE inst)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PinballArcadeFixBackdrop";
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                           mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                           nullptr, nullptr, inst, nullptr);
}

// Keeps the backdrop in step with the game window (minimise, restore, close).
static LRESULT CALLBACK GameProcHook(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        if (g_backdrop)
            ShowWindow(g_backdrop, w == SIZE_MINIMIZED ? SW_HIDE : SW_SHOWNOACTIVATE);
        break;
    case WM_NCDESTROY: {
        LRESULT r = CallWindowProcA(g_gameProc, h, m, w, l);
        if (g_backdrop) DestroyWindow(g_backdrop);
        g_backdrop = g_game = nullptr;
        return r;
    }
    }
    return CallWindowProcA(g_gameProc, h, m, w, l);
}

// ---------------------------------------------------------------- CreateWindowExA hook
typedef HWND(WINAPI* CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static CreateWindowExA_t g_realCreateWindowExA;

static HWND WINAPI CreateWindowExA_Hook(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w,
                                        int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    const bool isGameWindow = !IS_INTRESOURCE(cls) && lstrcmpA(cls, "GameWindowClass") == 0 && !g_game;
    if (!isGameWindow)
        return g_realCreateWindowExA(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);

    LoadDisplaySettings();
    if (g_mode == Mode::Off) {
        Log("Display: Mode=Off, leaving the game window alone");
        return g_realCreateWindowExA(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);
    }

    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT mon = mi.rcMonitor;
    const RECT r = FitRect(mon);

    g_backdrop = CreateBackdrop(mon, inst);
    // owned by the backdrop so it always stays in front of it; APPWINDOW keeps the taskbar button
    HWND hwnd = g_realCreateWindowExA(exStyle | WS_EX_APPWINDOW, cls, title, WS_POPUP | (style & WS_VISIBLE),
                                      r.left, r.top, r.right - r.left, r.bottom - r.top, g_backdrop, menu, inst,
                                      param);
    if (!hwnd) {
        if (g_backdrop) DestroyWindow(g_backdrop);
        g_backdrop = nullptr;
        Log("Display: game window creation failed (%lu)", GetLastError());
        return hwnd;
    }
    g_game = hwnd;
    g_gameProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)GameProcHook);
    Log("Display: monitor %ldx%ld, game %ldx%ld at %ld,%ld (aspect %d:%d)", mon.right - mon.left,
        mon.bottom - mon.top, r.right - r.left, r.bottom - r.top, r.left, r.top, g_aspectW, g_aspectH);
    return hwnd;
}

// ---------------------------------------------------------------- IAT patching
static bool PatchImport(HMODULE mod, const char* dll, const char* func, void* hook, void** orig)
{
    auto base = (BYTE*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *orig = (void*)addrs->u1.Function;
            addrs->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// The exe has no DPI-awareness manifest. Under Windows display scaling
// (e.g. a 4K monitor at 150%) it would be told the monitor is smaller and be
// bitmap-stretched by Windows, i.e. blurry. Declaring per-monitor DPI
// awareness before any window exists makes it render at true pixels.
static void MakeDpiAware()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetCtx_t)(HANDLE);
    if (auto setCtx = (SetCtx_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext")) {
        if (setCtx((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/) || setCtx((HANDLE)-3 /*PER_MONITOR_AWARE*/)) {
            Log("Display: per-monitor DPI aware");
            return;
        }
    }
    typedef BOOL(WINAPI * SetAware_t)();
    if (auto setAware = (SetAware_t)GetProcAddress(user32, "SetProcessDPIAware"); setAware && setAware())
        Log("Display: system DPI aware");
    else
        Log("Display: could not set DPI awareness (%lu)", GetLastError());
}

void InstallDisplayFix(HMODULE exe)
{
    MakeDpiAware();
    if (PatchImport(exe, "user32.dll", "CreateWindowExA", (void*)CreateWindowExA_Hook,
                    (void**)&g_realCreateWindowExA))
        Log("Display: CreateWindowExA hooked");
    else
        Log("Display: CreateWindowExA import not found, display fix inactive");
}
