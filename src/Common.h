#pragma once
#include <windows.h>

extern wchar_t g_dir[MAX_PATH]; // folder holding the DLL and its INI, with trailing backslash
void Log(const char* fmt, ...);
void IniPath(wchar_t (&out)[MAX_PATH]);

// Overwrites `len` prologue bytes at `at` (which must equal `expect`) with a
// jump to `to`; returns a trampoline that runs the original function, or null.
void* InstallDetour(BYTE* at, const void* to, const BYTE* expect, size_t len);

struct GameBuild {
    const wchar_t* exe;
    bool dx11;
    DWORD timestamp;      // PE header TimeDateStamp of the supported build
    DWORD frameTimer;     // DX11: RVA of the per-frame timer function
    DWORD setDisplayMode; // DX9: RVA of the set-display-mode function
    const char* windowClass;
    DWORD windowModeVar;  // DX9: RVA of the window-mode global (0 windowed, 1 fullscreen, 2 fullscreen window)
    DWORD uiTierVar;      // DX9: RVA of the UI quality tier (0 small, 1 >= 1280x720, 2 >= ~1680x1050)
    DWORD screenSizeVar;  // DX9: RVA of u16 width,height snapshot taken at window creation (menu background etc.)
    DWORD fullscreenKey;  // DX11: RVA of the "Fullscreen" settings-key string (read at one site only)
    DWORD presentSite;    // DX11: RVA of "push 1; push eax; call [ecx+20h]" (the Present call)
};

// Display fix (Display.cpp)
void InstallDisplayFix(HMODULE exe, const GameBuild& build);
