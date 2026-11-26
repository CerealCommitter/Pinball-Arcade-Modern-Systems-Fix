// Pinball Arcade fix: game speed.
//
// The DX11 build (PinballArcade11.exe) advances the simulation by a fixed
// 16 ms every rendered frame and has no frame cap, so the table runs 2x fast
// at 120 Hz, 4x at 240 Hz and faster still with vsync off. The DX9 build has
// a built-in 60 fps cap and is unaffected. We restore the 60 fps cadence the
// physics was tuned for by holding each DX11 frame to 1/60 s inside the
// game's own per-frame timer function.
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "winmm.lib")

static wchar_t g_dir[MAX_PATH];
static FILE* g_log;

static void Log(const char* fmt, ...)
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

static void LoadSettings()
{
    wchar_t ini[MAX_PATH];
    wsprintfW(ini, L"%sPinballArcadeFix.ini", g_dir);
    wchar_t buf[32];
    GetPrivateProfileStringW(L"Speed", L"FrameRateCap", L"60", buf, 32, ini);
    double v = _wtof(buf);
    if (v >= 0.0) g_fps = v; // 0 disables the cap
}

// ---------------------------------------------------------------- limiter
static LARGE_INTEGER g_freq, g_next;
static HANDLE g_timer;

static void LoadSettings();

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
    Log("Speed fix: frame cap active at %.2f fps", g_fps);
}

static void LimitFrame()
{
    static bool init = false;
    if (!init) { init = true; FirstFrameInit(); }
    if (g_fps <= 0.0) return;
    const LONGLONG period = (LONGLONG)(g_freq.QuadPart / g_fps);
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
struct GameBuild {
    const wchar_t* exe;
    DWORD timestamp;     // PE header TimeDateStamp of the supported build
    DWORD frameTimer;    // RVA of the per-frame timer function
    BYTE prologue[6];    // expected first bytes at frameTimer
};

// PinballArcade11.exe v1.71.30 (Steam build 5650517, Oct 2020)
static const GameBuild kDX11 = {
    L"pinballarcade11.exe", 0x5F7DE4C5, 0x0009D7B0, // VA 0x49D7B0 at base 0x400000
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14}, // push ebp; mov ebp,esp; sub esp,14h
};

typedef void(__cdecl* FrameTimer_t)();
static FrameTimer_t g_origFrameTimer;

static void __cdecl FrameTimerHook()
{
    LimitFrame();
    g_origFrameTimer();
}

static bool InstallJump(BYTE* at, const void* to, const BYTE* expect, size_t len)
{
    if (memcmp(at, expect, len) != 0) return false;
    // trampoline: original prologue + jmp back
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, at, len);
    tramp[len] = 0xE9;
    *(DWORD*)(tramp + len + 1) = (DWORD)((at + len) - (tramp + len + 5));
    g_origFrameTimer = (FrameTimer_t)tramp;

    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    at[0] = 0xE9;
    *(DWORD*)(at + 1) = (DWORD)((BYTE*)to - (at + 5));
    for (size_t i = 5; i < len; ++i) at[i] = 0x90;
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

static void InstallSpeedFix(HMODULE exe)
{
    auto dos = (IMAGE_DOS_HEADER*)exe;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kDX11.timestamp) {
        Log("Speed fix: unknown PinballArcade11.exe build (timestamp %08X), not patching",
            nt->FileHeader.TimeDateStamp);
        return;
    }
    // RVA from the module's actual load address (the in-memory ImageBase field
    // is rewritten by the loader on relocation, so it can't be used here)
    BYTE* fn = (BYTE*)exe + kDX11.frameTimer;
    if (InstallJump(fn, (void*)FrameTimerHook, kDX11.prologue, sizeof(kDX11.prologue)))
        Log("Speed fix: DX11 frame timer hooked");
    else
        Log("Speed fix: frame timer bytes did not match, not patching");
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

    if (_wcsicmp(exeName, kDX11.exe) == 0)
        InstallSpeedFix(GetModuleHandleW(nullptr));
    else
        Log("Speed fix: not needed for this exe (DX9 build already caps at 60 fps)");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
