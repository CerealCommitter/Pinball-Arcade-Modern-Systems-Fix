// Pinball Arcade fix: entry point, logging and the game-speed fix.
// (Screen resolution handling lives in Display.cpp.)
//
// Game speed:
//
// The DX11 build (PinballArcade11.exe) advances the simulation by a fixed
// 16 ms every rendered frame and has no frame cap, so the table runs 2x fast
// at 120 Hz, 4x at 240 Hz and faster still with vsync off. The DX9 build has
// a built-in 60 fps cap and is unaffected. We restore the 60 fps cadence the
// physics was tuned for.
//
// Pacing: a high-resolution timer inside the game's own per-frame timer
// function holds each frame to 1/60 s, and we take over the game's Present
// call to cap DXGI's frame queue at one frame so input stays responsive.
// (An opt-in Pacing=Refresh mode presents every Nth refresh instead; in this
// windowed swap chain it measured less even than the timer, so it is not the
// default.)
#include "Common.h"
#include <stdio.h>
#include <dxgi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dxguid.lib")

wchar_t g_dir[MAX_PATH];
static FILE* g_log;

void IniPath(wchar_t (&out)[MAX_PATH])
{
    wsprintfW(out, L"%sPinballArcadeFix.ini", g_dir);
}

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- settings
static double g_fps = 60.0;
static bool g_ioLog;               // [Debug] IoLog=1: per-250 ms read stats
static bool g_pacingTimerOnly = true; // [Speed] Pacing=Timer (default) | Refresh

static void LoadSettings()
{
    wchar_t ini[MAX_PATH];
    IniPath(ini);
    wchar_t buf[32];
    GetPrivateProfileStringW(L"Speed", L"FrameRateCap", L"60", buf, 32, ini);
    double v = _wtof(buf);
    if (v >= 0.0) g_fps = v; // 0 disables the cap
    GetPrivateProfileStringW(L"Speed", L"Pacing", L"Timer", buf, 32, ini);
    g_pacingTimerOnly = _wcsicmp(buf, L"Refresh") != 0;
    g_ioLog = GetPrivateProfileIntW(L"Debug", L"IoLog", 0, ini) != 0;
}

// ---------------------------------------------------------------- limiter
static LARGE_INTEGER g_freq, g_next;
static HANDLE g_timer;
static double g_limitFps;          // timer cap actually applied
static bool g_loading = true;      // startup loading in progress (see UpdateStartupLoading)
static LONGLONG g_startQpc;
static bool g_tableLoading;        // a table (or other content) is streaming in (see UpdateTableLoading)
volatile bool g_padPressed;        // set by the XInput proxy when a controller button is down
static HMODULE g_exe;
static const GameBuild* g_build;

static void LoadSettings();

// ---------------------------------------------------------------- present pacing
// We take over the game's one Present call (VA 0x73DB19: push 1 / push eax /
// call [ecx+20h], with the flags argument already pushed and the swap chain in
// eax) so we can (a) choose the sync interval and (b) cap DXGI's frame queue
// at one frame. Without (b), any time the game produces frames faster than
// the display consumes them, DXGI queues up to three, adding ~50 ms of input
// lag.
static int g_syncInterval = 1;     // decided on the first frame
static bool g_refreshLocked;

static int ChooseSyncInterval()
{
    if (g_fps <= 0.0) return 1;
    if (g_pacingTimerOnly) {
        Log("Speed fix: presenting every refresh, pacing with the timer");
        return 1;
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return 1;
    const double hz = dm.dmDisplayFrequency;
    const int n = (int)(hz / g_fps + 0.5);
    if (n < 1 || n > 4 || hz - n * g_fps > 1.5 || n * g_fps - hz > 1.5) {
        Log("Speed fix: %u Hz is not a multiple of %.0f fps, pacing with the timer", dm.dmDisplayFrequency, g_fps);
        return 1;
    }
    Log("Speed fix: %u Hz display, presenting every %d refresh(es) for an even %.0f fps", dm.dmDisplayFrequency, n,
        g_fps);
    g_refreshLocked = true;
    return n;
}

static IDXGISwapChain* g_swap;

static int __cdecl OnPresent(IDXGISwapChain* swap)
{
    static IDXGISwapChain* configured;
    if (swap != configured) { // first frame, or the game recreated its device
        configured = swap;
        IDXGIDevice1* dev = nullptr;
        if (SUCCEEDED(swap->GetDevice(__uuidof(IDXGIDevice1), (void**)&dev)) && dev) {
            dev->SetMaximumFrameLatency(1);
            dev->Release();
            Log("Speed fix: DXGI frame queue limited to 1");
        }
    }
    g_swap = swap;
    return (g_loading || g_tableLoading) ? 0 : g_syncInterval;
}

// Entered by "call" from the patched site: [esp]=return, [esp+4]=flags, eax=swap chain.
static __declspec(naked) void PresentThunk()
{
    __asm {
        push eax
        call OnPresent           ; eax = sync interval
        add esp, 4
        mov ecx, [esp + 4]       ; flags
        push ecx
        push eax
        mov eax, g_swap
        push eax
        mov ecx, [eax]
        call dword ptr [ecx + 0x20]  ; IDXGISwapChain::Present (stdcall, pops 3)
        ret 4                    ; also drop the flags the game pushed
    }
}

static bool InstallPresentHook()
{
    if (!g_build || !g_build->presentSite) return false;
    BYTE* at = (BYTE*)g_exe + g_build->presentSite;
    static const BYTE expect[] = {0x6A, 0x01, 0x50, 0xFF, 0x51, 0x20}; // push 1; push eax; call [ecx+20h]
    if (memcmp(at, expect, sizeof(expect)) != 0) return false;
    DWORD old;
    VirtualProtect(at, sizeof(expect), PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE8;
    *(DWORD*)(at + 1) = (DWORD)((BYTE*)PresentThunk - (at + 5));
    at[5] = 0x90;
    VirtualProtect(at, sizeof(expect), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(expect));
    return true;
}

// Settings and the timer are set up on the first frame, not in DllMain:
// reading the INI under the loader lock can deadlock the game at startup.
static void FirstFrameInit()
{
    LoadSettings();
    QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_startQpc = t.QuadPart;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!g_timer) {
        timeBeginPeriod(1);
        Log("High-resolution timer unavailable, using Sleep with 1 ms period");
    }
    g_syncInterval = ChooseSyncInterval();
    // refresh-locked: vsync paces, the timer only backstops a driver forcing vsync off
    g_limitFps = g_refreshLocked ? g_fps * 1.02 : g_fps;
    if (g_fps > 0.0)
        Log("Speed fix: timer cap at %.2f fps", g_limitFps);
    else
        Log("Speed fix: FrameRateCap=0, cap disabled");
}

// Frame-interval statistics, logged every ~10 s, to diagnose pacing.
static void FrameStats()
{
    static LARGE_INTEGER last;
    static double sum, worst;
    static int frames, slow;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        const double ms = (now.QuadPart - last.QuadPart) * 1000.0 / g_freq.QuadPart;
        sum += ms;
        if (ms > worst) worst = ms;
        if (ms > 20.0) ++slow;
        if (++frames == 600) {
            Log("Frames: avg %.2f ms (%.1f fps), worst %.1f ms, %d of 600 over 20 ms", sum / frames,
                1000.0 * frames / sum, worst, slow);
            sum = worst = 0;
            frames = slow = 0;
        }
    }
    last = now;
}

// ---------------------------------------------------------------- startup loading
// The game streams its startup data in one chunk per frame and only accepts
// input on "Press Start" once that is done. Capped at 60 fps this took 20-45 s
// (the title screen seemed frozen and ignored clicks and keys). Until the
// startup reads go quiet and the player presses something (mouse, keyboard or
// controller), we lift the cap and present without vsync: the title screen is
// all that's showing, and the game is ready for that first press within a
// couple of seconds; the cap returns the moment they press. A 20 s limit ends
// the phase if nobody presses anything. This applies to startup only;
// lifting the cap later (table loads, or a disk read mid-game) could
// fast-forward a table in play.
static void UpdateStartupLoading()
{
    static ULONGLONG lastBytes, windowBytes;
    static LONGLONG windowStart;
    static int quietWindows;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    IO_COUNTERS io;
    if (!GetProcessIoCounters(GetCurrentProcess(), &io)) {
        g_loading = false;
        return;
    }
    if (!windowStart) {
        windowStart = now.QuadPart;
        lastBytes = io.ReadTransferCount;
        return;
    }
    // has the player pressed anything (key, mouse button or controller) since loading went quiet?
    static LONGLONG lastInputCheck;
    static bool pressed, quiet;
    if (quiet && !pressed && now.QuadPart - lastInputCheck > g_freq.QuadPart / 100) {
        lastInputCheck = now.QuadPart;
        for (int vk = 1; vk < 0xFF && !pressed; ++vk)
            if (vk != VK_MENU && vk != VK_LMENU && vk != VK_RMENU && (GetAsyncKeyState(vk) & 0x8000)) pressed = true;
        if (g_padPressed) pressed = true;
        if (pressed) {
            g_loading = false;
            g_next.QuadPart = 0;
            Log("Speed fix: startup phase over after %.1f s (player input), 60 fps cap active",
                (double)(now.QuadPart - g_startQpc) / g_freq.QuadPart);
            return;
        }
    }
    windowBytes += io.ReadTransferCount - lastBytes;
    lastBytes = io.ReadTransferCount;
    if (now.QuadPart - windowStart < g_freq.QuadPart / 4) return; // 250 ms windows
    const bool busy = windowBytes > 256 * 1024;
    windowBytes = 0;
    windowStart = now.QuadPart;
    quietWindows = busy ? 0 : quietWindows + 1;
    const double secs = (double)(now.QuadPart - g_startQpc) / g_freq.QuadPart;
    if (quietWindows >= 2 && !quiet) {
        quiet = true;
        g_padPressed = false; // only presses from here on count
        Log("Speed fix: startup reads finished after %.1f s", secs);
    }
    if (secs > 20.0) {
        g_loading = false;
        g_next.QuadPart = 0; // resync the cap
        Log("Speed fix: startup phase over after %.1f s (time limit), 60 fps cap active", secs);
    }
}

// ---------------------------------------------------------------- table loading
// Table loads stream the same way: measured on one load, the game read from
// disk on 13-16 of every 16 frames for ~25 s at 60 fps, while gameplay itself
// (game start, flippers, scoring) read nothing at all. So, per frame: when at
// least 12 of the last 16 frames read from disk we lift the cap; after 8
// frames in a row without reads it is back. At uncapped speed that tail is a
// few milliseconds, ~0.1 s of game time on a table that is sitting idle.
static void UpdateTableLoading()
{
    static ULONGLONG lastBytes;
    static bool primed;
    static unsigned history;     // bit i = frame i ago read from disk
    static int quietRun;
    IO_COUNTERS io;
    if (!GetProcessIoCounters(GetCurrentProcess(), &io)) return;
    if (!primed) {
        primed = true;
        lastBytes = io.ReadTransferCount;
        return;
    }
    const bool read = io.ReadTransferCount != lastBytes;
    lastBytes = io.ReadTransferCount;
    history = ((history << 1) | (read ? 1u : 0u)) & 0xFFFF;
    quietRun = read ? 0 : quietRun + 1;

    int recent = 0;
    for (unsigned h = history; h; h &= h - 1) ++recent;
    // reads come in bursts, so the cap toggles a few times per load; log once per load
    static LONGLONG burstStart, lastOff;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_tableLoading && recent >= 12) {
        g_tableLoading = true;
        if (!burstStart || now.QuadPart - lastOff > g_freq.QuadPart * 2) burstStart = now.QuadPart;
    } else if (g_tableLoading && quietRun >= 8) {
        g_tableLoading = false;
        g_next.QuadPart = 0; // resync the cap
        history = 0;
        lastOff = now.QuadPart;
    }
    if (burstStart && !g_tableLoading && now.QuadPart - lastOff > g_freq.QuadPart * 2) {
        Log("Speed fix: content load ran uncapped for %.1f s", (double)(lastOff - burstStart) / g_freq.QuadPart);
        burstStart = 0;
    }
}

static void IoLogFrame()
{
    static ULONGLONG lastBytes, lastOps, bytes, ops;
    static LONGLONG start;
    static int frames, readFrames;
    IO_COUNTERS io;
    if (!GetProcessIoCounters(GetCurrentProcess(), &io)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!start) { start = now.QuadPart; lastBytes = io.ReadTransferCount; lastOps = io.ReadOperationCount; return; }
    const ULONGLONG db = io.ReadTransferCount - lastBytes;
    if (db) ++readFrames;
    bytes += db;
    ops += io.ReadOperationCount - lastOps;
    lastBytes = io.ReadTransferCount;
    lastOps = io.ReadOperationCount;
    ++frames;
    if (now.QuadPart - start >= g_freq.QuadPart / 4) {
        Log("io %.2f s: %llu bytes, %llu ops, %d frames (%d with reads)%s",
            (double)(now.QuadPart - g_startQpc) / g_freq.QuadPart, bytes, ops, frames, readFrames,
            g_loading ? " [startup]" : "");
        start = now.QuadPart;
        bytes = ops = 0;
        frames = readFrames = 0;
    }
}

static void LimitFrame()
{
    static bool init = false;
    if (!init) { init = true; FirstFrameInit(); }
    if (g_ioLog) IoLogFrame();
    if (g_loading) {
        UpdateStartupLoading();
        return;
    }
    UpdateTableLoading();
    if (g_limitFps <= 0.0 || g_tableLoading) return;
    const LONGLONG period = (LONGLONG)(g_freq.QuadPart / g_limitFps);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_next.QuadPart == 0 || now.QuadPart - g_next.QuadPart > period * 4) {
        // first frame, or we fell far behind (loading screen): resync
        g_next.QuadPart = now.QuadPart + period;
        return;
    }
    LONGLONG remain = g_next.QuadPart - now.QuadPart;
    if (remain > 0) {
        // sleep for most of it, spin the last ~1.5 ms for precision
        LONGLONG sleep100ns = (remain * 10000000LL) / g_freq.QuadPart - 15000;
        if (sleep100ns > 0) {
            if (g_timer) {
                LARGE_INTEGER due;
                due.QuadPart = -sleep100ns;
                SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(g_timer, INFINITE);
            } else {
                Sleep((DWORD)(sleep100ns / 10000));
            }
        }
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < g_next.QuadPart);
    }
    g_next.QuadPart += period;
}

// ---------------------------------------------------------------- hooking
void* InstallDetour(BYTE* at, const void* to, const BYTE* expect, size_t len)
{
    if (len < 5 || memcmp(at, expect, len) != 0) return nullptr;
    // trampoline: original prologue + jmp back (prologues used here hold no relative operands)
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, at, len);
    tramp[len] = 0xE9;
    *(DWORD*)(tramp + len + 1) = (DWORD)((at + len) - (tramp + len + 5));

    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    at[0] = 0xE9;
    *(DWORD*)(at + 1) = (DWORD)((BYTE*)to - (at + 5));
    for (size_t i = 5; i < len; ++i) at[i] = 0x90;
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return tramp;
}

// Supported builds: v1.71.30 (Steam build 5650517, Oct 2020). Addresses are
// RVAs from the module's actual load address (the in-memory ImageBase field is
// rewritten by the loader on relocation, so it can't be used to rebase VAs).
static const GameBuild kBuilds[] = {
    // DX11: frame timer at VA 0x49D7B0; Present call site at VA 0x73DB19; "Fullscreen" key
    // string at VA 0xAC1A94. Overlay fix: menu id VA 0x16982C0, EnterInitials VA 0x110E0EC,
    // table VA 0xF0A8FC, menus getter VA 0x419AC0, menu back VA 0x68F410.
    {L"pinballarcade11.exe", true, 0x5F7DE4C5, 0x0009D7B0, 0x14, 0, "GameWindowClass", 0, 0, 0, 0x006C1A94,
     0x0033DB19, 0x012982C0, 0x00D0E0EC, 0x00B0A8FC, 0x00019AC0, 0x0028F410},
    // DX9: frame timer at VA 0x5BED70 (holds its own 60 fps busy-wait); set-display-mode at VA
    // 0x5BA5A0; window mode at VA 0xD072C8; UI tier at VA 0xB46430; screen-size snapshot at VA
    // 0xD41A10. Overlay fix: menu id VA 0x13BD8A8, EnterInitials VA 0xE45A6C, table VA
    // 0xB463F8, menus getter VA 0x406C30, menu back VA 0x669530.
    {L"pinballarcade.exe", false, 0x5F7DE48A, 0x001BED70, 0x18, 0x001BA5A0, "Pinball Arcade", 0x009072C8,
     0x00746430, 0x00941A10, 0, 0, 0x00FBD8A8, 0x00A45A6C, 0x007463F8, 0x00006C30, 0x00269530},
};

// ---------------------------------------------------------------- stuck initials overlay
// Original game bug (reported by players long before this fix): after entering initials on
// an emulated-ROM table such as Gopher, the on-screen "Select" overlay (GUI menu 0x23) stays
// up through the next game and captures the flipper keys. The game shows that menu while a
// table's display matches its "EnterInitials" pattern, but only some table types have the
// code that closes it again ("if menu 0x23 and entry finished: go back a menu", e.g. VA
// 0x4C33BA). For the rest it is never closed. We do the same thing generically: if the
// EnterName menu has been open for 0.5 s on a ROM-initials table while the game's
// EnterInitials flag is clear, call the game's own "back to previous menu" (VA 0x68F410).
const DWORD kRomInitials = 0x310FC; // table field: initials are entered through the ROM
const int kEnterNameMenu = 0x23;

static void FixStuckInitialsOverlay()
{
    if (!g_build || !g_build->curMenuVar) return;
    BYTE* exe = (BYTE*)g_exe;
    static int stuckFrames;
    const int menu = *(int*)(exe + g_build->curMenuVar);
    BYTE* table = *(BYTE**)(exe + g_build->tablePtrVar);
    const bool stuck = menu == kEnterNameMenu && table && *(int*)(table + kRomInitials) &&
                       *(int*)(exe + g_build->enterInitialsVar) == 0;
    if (!stuck) {
        stuckFrames = 0;
        return;
    }
    if (++stuckFrames < 30) return; // 0.5 s at 60 fps
    stuckFrames = 0;
    typedef void*(__cdecl * GetMenus_t)();
    typedef void(__thiscall * MenuBack_t)(void*);
    void* menus = ((GetMenus_t)(exe + g_build->getMenus))();
    if (!menus) return;
    ((MenuBack_t)(exe + g_build->menuBack))(menus);
    Log("Game fix: closed the initials overlay left open after high-score entry");
}

typedef void(__cdecl* FrameTimer_t)();
static FrameTimer_t g_origFrameTimer;

static void __cdecl FrameTimerHook()
{
    if (g_build->dx11) {
        LimitFrame();
        FrameStats();
    }
    FixStuckInitialsOverlay();
    g_origFrameTimer();
}

// Per-frame hook in the game's own frame timer: the DX11 frame cap plus game fixes (both builds).
static void InstallFrameHook(HMODULE exe, const GameBuild& b)
{
    if (b.dx11)
        Log(InstallPresentHook() ? "Speed fix: Present hooked" : "Speed fix: Present bytes did not match, not hooked");
    const BYTE prologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, b.frameTimerStack};
    g_origFrameTimer = (FrameTimer_t)InstallDetour((BYTE*)exe + b.frameTimer, (void*)FrameTimerHook, prologue,
                                                   sizeof(prologue));
    if (!g_origFrameTimer)
        Log("Frame hook: frame timer bytes did not match, not hooked (speed and overlay fixes inactive)");
    else if (b.dx11)
        Log("Speed fix: DX11 frame timer hooked");
    else
        Log("Frame hook: DX9 frame timer hooked (game fixes only; DX9 already caps at 60 fps)");
}

// ---------------------------------------------------------------- entry
static void Init()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Init, &self);
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) slash[1] = 0;

    wchar_t logPath[MAX_PATH];
    wsprintfW(logPath, L"%sPinballArcadeFix.log", g_dir);
    g_log = _wfopen(logPath, L"w");

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exePath, L'\\') ? wcsrchr(exePath, L'\\') + 1 : exePath;
    Log("Pinball Arcade Fix loaded into %ls", exeName);

    HMODULE exe = GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    for (const GameBuild& b : kBuilds) {
        if (_wcsicmp(exeName, b.exe) != 0) continue;
        if (nt->FileHeader.TimeDateStamp != b.timestamp) {
            Log("Unknown %ls build (timestamp %08X), not patching", exeName, nt->FileHeader.TimeDateStamp);
            return;
        }
        g_exe = exe;
        g_build = &b;
        InstallFrameHook(exe, b);
        InstallDisplayFix(exe, b);
        InstallLauncher(exe);
        return;
    }
    Log("Not a supported game exe, doing nothing");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
