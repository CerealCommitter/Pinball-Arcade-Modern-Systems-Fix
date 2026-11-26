// Pinball Arcade fix: launch settings window.
//
// Replaces the game's "Launch Configuration Utility" (PBAConfig.exe), which crashes on a fresh
// install (it parses blank cabinet-layout fields as numbers) and offers resolution / window-mode
// options that no longer apply with this fix. The game's entry point is redirected here, so the
// window runs on the main thread before the game reads its settings, from whichever Steam
// launch option (DX9 or DX11) was used.
//
// On Play we write the same files PBAConfig wrote, in the same formats, so the game picks the
// settings up unchanged:
//   settings.dat       11 little-endian int32s: version, post-processing, window mode, width,
//                      height, anti-aliasing, ball reflection, language (-1 = default),
//                      monitor, ignore input when unfocused, texture filter. (Read by both
//                      builds.)
//   dx11_settings.dat  hex text of records "name\0" + int64 big-endian + "\0" (DX11 only):
//                      MSAAS, MSAAQ, ANISO, BallBrighten, MultiScreen, Fullscreen,
//                      BackBufferW/H; the game itself keeps WindowX/Y/W/H in the same file.
// plus our own options in PinballArcadeFix.ini. Cabinet (Arcooda) settings are left alone.
#include "Common.h"
#include "LauncherUI.h"

#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- game settings files
struct GameSettings {
    // settings.dat
    int version = 4, postProcessing = 0, windowMode = 0, width = 640, height = 480;
    int antiAliasing = 0, ballReflection = 1, language = -1, monitor = 0, ignoreInput = 1,
        textureFilter = 0;
    // dx11_settings.dat (ordered records, kept as found plus our keys)
    std::vector<std::pair<std::string, long long>> dx11;
    long long Dx11(const char* k, long long def) const {
        for (auto& r : dx11)
            if (r.first == k) return r.second;
        return def;
    }
    void SetDx11(const char* k, long long v) {
        for (auto& r : dx11)
            if (r.first == k) { r.second = v; return; }
        dx11.emplace_back(k, v);
    }
};

std::wstring SaveFolder()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PinballArcade\\PinballArcade", L"SavePath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0]) {
        std::wstring s = buf;
        if (s.back() != L'\\') s += L'\\';
        return s;
    }
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return std::wstring(buf) + L"\\My Games\\Pinball Arcade\\";
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size)
{
    // write to a temp file and swap it in, like PBAConfig did
    const std::wstring tmp = path + L".temp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void LoadGameSettings(const std::wstring& folder, GameSettings& g)
{
    std::vector<unsigned char> b;
    if (ReadFileBytes(folder + L"settings.dat", b) && b.size() >= 44) {
        int v[11];
        memcpy(v, b.data(), sizeof(v));
        g.version = v[0];
        g.postProcessing = v[1];
        g.windowMode = v[2];
        g.width = v[3];
        g.height = v[4];
        g.antiAliasing = v[5];
        g.ballReflection = v[6];
        g.language = v[7];
        g.monitor = v[8];
        g.ignoreInput = v[9];
        g.textureFilter = v[10];
    }
    std::vector<unsigned char> hex;
    if (ReadFileBytes(folder + L"dx11_settings.dat", hex)) {
        std::vector<unsigned char> raw;
        auto nib = [](unsigned char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
            if (hi < 0 || lo < 0) break;
            raw.push_back((unsigned char)(hi << 4 | lo));
        }
        size_t i = 0;
        while (i < raw.size()) {
            size_t z = i;
            while (z < raw.size() && raw[z]) ++z;
            if (z == i || z + 10 > raw.size()) break;
            long long v = 0;
            for (int k = 0; k < 8; ++k) v = (v << 8) | raw[z + 1 + k];
            g.dx11.emplace_back(std::string((const char*)&raw[i], z - i), v);
            i = z + 10; // name, NUL, 8 value bytes, NUL
        }
    }
}

bool SaveGameSettings(const std::wstring& folder, const GameSettings& g)
{
    CreateDirectoryW(folder.c_str(), nullptr);
    const int v[11] = {g.version,      g.postProcessing, g.windowMode, g.width,
                       g.height,       g.antiAliasing,   g.ballReflection, g.language,
                       g.monitor,      g.ignoreInput,    g.textureFilter};
    bool ok = WriteFileBytes(folder + L"settings.dat", v, sizeof(v));

    std::string hex;
    static const char* digits = "0123456789ABCDEF";
    auto put = [&](unsigned char c) {
        hex += digits[c >> 4];
        hex += digits[c & 15];
    };
    for (auto& r : g.dx11) {
        for (char c : r.first) put((unsigned char)c);
        put(0);
        for (int k = 7; k >= 0; --k) put((unsigned char)(r.second >> (k * 8)));
        put(0);
    }
    ok = WriteFileBytes(folder + L"dx11_settings.dat", hex.data(), hex.size()) && ok;
    return ok;
}

// ---------------------------------------------------------------- choices
// Each launcher choice maps onto one or both game settings files.
const int kAaDx9[] = {0, 2, 3};        // settings.dat AntiAliasing index: None, 4X, 8X
const int kAaDx11[] = {1, 4, 8};       // dx11 MSAAS samples
const int kTexDx9[] = {1, 2, 3, 4};    // settings.dat TextureFilter: Linear, Aniso x4/x8/x16
const int kTexDx11[] = {1, 4, 8, 16};  // dx11 ANISO

template <size_t N>
int IndexOf(const int (&table)[N], long long value, int fallback)
{
    for (size_t i = 0; i < N; ++i)
        if (table[i] == value) return (int)i;
    return fallback;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Largest 16:9 size on the primary monitor (what the display fix renders at).
void FittedSize(int& w, int& h)
{
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    w = mw;
    h = mh;
    if ((long long)mw * 9 > (long long)mh * 16) w = mh * 16 / 9;
    else h = mw * 9 / 16;
}

// ---------------------------------------------------------------- renderer switch
// Steam asks which launch option to run unless the player ticks "remember my choice"; with
// that set, the Renderer choice here lets them switch between the DX11 and DX9 builds anyway.
// We start the other exe ourselves (it inherits Steam's environment, including SteamAppId, so
// it connects to Steam normally) and mark it so its launcher doesn't show a second time.
const wchar_t* kRelaunchedVar = L"PINBALLARCADEFIX_RELAUNCHED";

bool CurrentIsDx11()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    return _wcsicmp(name, L"PinballArcade11.exe") == 0;
}

// Starts the other build with the same arguments; returns false if it couldn't be started.
bool StartOtherRenderer(bool dx11)
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) slash[1] = 0;
    const std::wstring exe = std::wstring(dir) + (dx11 ? L"PinballArcade11.exe" : L"PinballArcade.exe");

    // keep the original arguments: skip the (possibly quoted) program name in our command line
    const wchar_t* args = GetCommandLineW();
    if (*args == L'"') {
        ++args;
        while (*args && *args != L'"') ++args;
        if (*args) ++args;
    } else {
        while (*args && *args != L' ' && *args != L'\t') ++args;
    }
    std::wstring cmd = L"\"" + exe + L"\"" + args;

    SetEnvironmentVariableW(kRelaunchedVar, L"1");
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi);
    SetEnvironmentVariableW(kRelaunchedVar, nullptr);
    if (!ok) {
        Log("Launcher: could not start %ls (error %lu), staying on this renderer", exe.c_str(), GetLastError());
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId); // let the new game window come to the front
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log("Launcher: switched to %ls", dx11 ? L"DirectX 11" : L"DirectX 9");
    return true;
}

}  // namespace

// ---------------------------------------------------------------- launcher
// Returns false if the player chose Quit.
bool RunLauncher()
{
    wchar_t ini[MAX_PATH];
    IniPath(ini);

    // started by our own renderer switch: the player already chose everything
    if (GetEnvironmentVariableW(kRelaunchedVar, nullptr, 0)) {
        SetEnvironmentVariableW(kRelaunchedVar, nullptr);
        Log("Launcher: skipped (started by the renderer switch)");
        return true;
    }

    const bool isDx11 = CurrentIsDx11();
    wchar_t renderer[16];
    // DX11 is the default until the player picks something else
    GetPrivateProfileStringW(L"Launcher", L"Renderer", L"DX11", renderer, 16, ini);
    const bool wantDx11 = _wcsicmp(renderer, L"DX9") != 0;

    const bool showAtLaunch = GetPrivateProfileIntW(L"Launcher", L"ShowAtLaunch", 1, ini) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!showAtLaunch && !shift) {
        Log("Launcher: skipped (ShowAtLaunch=0; hold Shift while starting to open it)");
        if (wantDx11 != isDx11 && StartOtherRenderer(wantDx11)) ExitProcess(0);
        return true;
    }

    const std::wstring folder = SaveFolder();
    GameSettings g;
    LoadGameSettings(folder, g);

    wchar_t mode[32];
    GetPrivateProfileStringW(L"Display", L"Mode", L"Fullscreen", mode, 32, ini);
    const bool original = _wcsicmp(mode, L"Off") == 0 || _wcsicmp(mode, L"Original") == 0;

    enum { cRenderer, cDisplay, cAA, cTex, cPost, cReflect, cBright, cLang, cInput };
    LauncherSpec spec;
    spec.windowTitle = L"Pinball Arcade - Modern Systems Fix";
    spec.gameTitle = L"Pinball Arcade";
    spec.subtitle = L"Modern Systems Fix";
    spec.choices = {
        {L"Renderer", {L"DirectX 11 (recommended)", L"DirectX 9"}, wantDx11 ? 0 : 1},
        {L"Display", {L"Fullscreen 16:9 with black bars", L"Original game window (fix off)"}, original ? 1 : 0},
        {L"Anti-aliasing", {L"Off", L"4x", L"8x"},
         IndexOf(kAaDx11, g.Dx11("MSAAS", kAaDx11[IndexOf(kAaDx9, g.antiAliasing, 0)]), 0)},
        {L"Texture filtering", {L"Bilinear", L"Anisotropic 4x", L"Anisotropic 8x", L"Anisotropic 16x"},
         IndexOf(kTexDx11, g.Dx11("ANISO", kTexDx11[IndexOf(kTexDx9, g.textureFilter, 3)]), 3)},
        {L"Post-processing (DX9)", {L"Off", L"On"}, Clamp(g.postProcessing, 0, 1)},
        {L"Ball reflection (DX9)", {L"Off", L"Low", L"High"}, Clamp(g.ballReflection, 0, 2)},
        {L"Ball brightening (DX11)", {L"Off", L"On"}, (int)Clamp((int)g.Dx11("BallBrighten", 0), 0, 1)},
        {L"Language", {L"Game default", L"English", L"French", L"Italian", L"German", L"Spanish"},
         Clamp(g.language + 1, 0, 5)},
        {L"When the game is in the background", {L"Ignore keyboard and controller", L"Keep reading input"},
         g.ignoreInput ? 0 : 1},
    };
    spec.checkboxText = L"Show this window every time the game starts";
    spec.hintText = L"If you turn this off, hold Shift while the game starts to bring it back.";
    spec.checked = showAtLaunch;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&RunLauncher, &self);
    if (!ShowLauncher(self, spec)) {
        Log("Launcher: Quit");
        return false;
    }

    // ---- apply
    const auto sel = [&](int i) { return spec.choices[i].selected; };
    const bool fixOn = sel(cDisplay) == 0;
    WritePrivateProfileStringW(L"Display", L"Mode", fixOn ? L"Fullscreen" : L"Off", ini);
    WritePrivateProfileStringW(L"Launcher", L"ShowAtLaunch", spec.checked ? L"1" : L"0", ini);

    g.version = 4;
    g.postProcessing = sel(cPost);
    g.antiAliasing = kAaDx9[sel(cAA)];
    g.ballReflection = sel(cReflect);
    g.language = sel(cLang) - 1;
    g.ignoreInput = sel(cInput) == 0 ? 1 : 0;
    g.textureFilter = kTexDx9[sel(cTex)];
    g.SetDx11("MSAAS", kAaDx11[sel(cAA)]);
    g.SetDx11("MSAAQ", g.Dx11("MSAAQ", 0));
    g.SetDx11("ANISO", kTexDx11[sel(cTex)]);
    g.SetDx11("BallBrighten", sel(cBright));
    g.SetDx11("MultiScreen", g.Dx11("MultiScreen", 0));
    if (fixOn) {
        // windowed at the size the display fix renders at; the fix keeps it there regardless
        int w, h;
        FittedSize(w, h);
        g.windowMode = 0;
        g.width = w;
        g.height = h;
        g.monitor = 0;
        g.SetDx11("Fullscreen", 0);
        g.SetDx11("BackBufferW", w);
        g.SetDx11("BackBufferH", h);
    }
    const bool ok = SaveGameSettings(folder, g);
    Log("Launcher: Play; settings %s (AA %d, texture filter %d, post %d, reflection %d, brighten %d, "
        "language %d, ignore input %d, display fix %s)",
        ok ? "saved" : "could NOT be saved", g.antiAliasing, g.textureFilter, g.postProcessing,
        g.ballReflection, sel(cBright), g.language, g.ignoreInput, fixOn ? "on" : "off");

    const bool chooseDx11 = sel(cRenderer) == 0;
    WritePrivateProfileStringW(L"Launcher", L"Renderer", chooseDx11 ? L"DX11" : L"DX9", ini);
    if (chooseDx11 != isDx11 && StartOtherRenderer(chooseDx11)) ExitProcess(0);
    return true;
}

// ---------------------------------------------------------------- entry-point hook
// The window must run before the game reads settings.dat, i.e. before its WinMain. We
// overwrite the exe's entry point with a jump to EntryThunk, show the window there, put the
// original bytes back and continue into the real entry point.
static BYTE* g_entry;
static BYTE g_entryBytes[5];

static void* __cdecl AtEntry()
{
    if (!RunLauncher()) ExitProcess(0);
    DisplayFixBeforeGameStart();
    DWORD old;
    VirtualProtect(g_entry, sizeof(g_entryBytes), PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_entry, g_entryBytes, sizeof(g_entryBytes));
    VirtualProtect(g_entry, sizeof(g_entryBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_entry, sizeof(g_entryBytes));
    return g_entry;
}

static __declspec(naked) void EntryThunk()
{
    __asm {
        call AtEntry
        jmp eax
    }
}

void InstallLauncher(HMODULE exe)
{
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_entry = (BYTE*)exe + nt->OptionalHeader.AddressOfEntryPoint;
    memcpy(g_entryBytes, g_entry, sizeof(g_entryBytes));
    DWORD old;
    if (!VirtualProtect(g_entry, sizeof(g_entryBytes), PAGE_EXECUTE_READWRITE, &old)) return;
    g_entry[0] = 0xE9;
    *(DWORD*)(g_entry + 1) = (DWORD)((BYTE*)EntryThunk - (g_entry + 5));
    VirtualProtect(g_entry, sizeof(g_entryBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_entry, sizeof(g_entryBytes));
    Log("Launcher: entry point hooked");
}
