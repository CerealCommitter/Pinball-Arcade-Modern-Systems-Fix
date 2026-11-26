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
}

// ---------------------------------------------------------------- limiter
static LARGE_INTEGER g_freq, g_next;
static HANDLE g_timer;
static double g_limitFps;          // timer cap actually applied
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
    return g_syncInterval;
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

static void LimitFrame()
{
    static bool init = false;
    if (!init) { init = true; FirstFrameInit(); }
    if (g_limitFps <= 0.0) return;
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
    // DX11: frame timer at VA 0x49D7B0 (push ebp; mov ebp,esp; sub esp,14h)
    // Present call site at VA 0x73DB19
    {L"pinballarcade11.exe", true, 0x5F7DE4C5, 0x0009D7B0, 0, "GameWindowClass", 0, 0x0033DB19},
    // DX9: set-display-mode at VA 0x5BA5A0; window mode global at VA 0xD072C8
    {L"pinballarcade.exe", false, 0x5F7DE48A, 0, 0x001BA5A0, "Pinball Arcade", 0x009072C8, 0},
};

typedef void(__cdecl* FrameTimer_t)();
static FrameTimer_t g_origFrameTimer;

static void __cdecl FrameTimerHook()
{
    LimitFrame();
    FrameStats();
    g_origFrameTimer();
}

static void InstallSpeedFix(HMODULE exe, const GameBuild& b)
{
    g_exe = exe;
    g_build = &b;
    Log(InstallPresentHook() ? "Speed fix: Present hooked" : "Speed fix: Present bytes did not match, not hooked");
    static const BYTE prologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
    g_origFrameTimer = (FrameTimer_t)InstallDetour((BYTE*)exe + b.frameTimer, (void*)FrameTimerHook, prologue,
                                                   sizeof(prologue));
    Log(g_origFrameTimer ? "Speed fix: DX11 frame timer hooked"
                         : "Speed fix: frame timer bytes did not match, not patching");
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
        if (b.dx11)
            InstallSpeedFix(exe, b);
        else
            Log("Speed fix: not needed (DX9 build has its own 60 fps cap)");
        InstallDisplayFix(exe, b);
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
