// Forwards every xinput1_3.dll export to the real system DLL so the game's
// controller support keeps working while our fix rides along in DllMain.
#include <windows.h>

static HMODULE g_real;

static FARPROC Real(LPCSTR name)
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH); // SysWOW64 for a 32-bit process
        lstrcatW(path, L"\\xinput1_3.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

#define FWD(ret, fail, name, lookup, params, args)                        \
    extern "C" ret WINAPI Proxy_##name params                              \
    {                                                                      \
        typedef ret(WINAPI * fn_t) params;                                 \
        static fn_t fn = (fn_t)Real(lookup);                               \
        return fn ? fn args : fail;                                        \
    }

FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, XInputGetState, "XInputGetState", (DWORD a, void* b), (a, b))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, XInputSetState, "XInputSetState", (DWORD a, void* b), (a, b))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, XInputGetCapabilities, "XInputGetCapabilities", (DWORD a, DWORD b, void* c), (a, b, c))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, XInputGetDSoundAudioDeviceGuids, "XInputGetDSoundAudioDeviceGuids", (DWORD a, GUID* b, GUID* c), (a, b, c))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, XInputGetBatteryInformation, "XInputGetBatteryInformation", (DWORD a, BYTE b, void* c), (a, b, c))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, XInputGetKeystroke, "XInputGetKeystroke", (DWORD a, DWORD b, void* c), (a, b, c))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, Ord100, MAKEINTRESOURCEA(100), (DWORD a, void* b), (a, b))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, Ord101, MAKEINTRESOURCEA(101), (DWORD a, DWORD b, void* c), (a, b, c))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, Ord102, MAKEINTRESOURCEA(102), (DWORD a), (a))
FWD(DWORD, ERROR_DEVICE_NOT_CONNECTED, Ord103, MAKEINTRESOURCEA(103), (DWORD a), (a))

extern "C" void WINAPI Proxy_XInputEnable(BOOL enable)
{
    typedef void(WINAPI * fn_t)(BOOL);
    static fn_t fn = (fn_t)Real("XInputEnable");
    if (fn) fn(enable);
}
