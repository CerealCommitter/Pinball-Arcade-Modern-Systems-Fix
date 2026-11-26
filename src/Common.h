#pragma once
#include <windows.h>

extern wchar_t g_dir[MAX_PATH]; // folder holding the DLL and its INI, with trailing backslash
void Log(const char* fmt, ...);
void IniPath(wchar_t (&out)[MAX_PATH]);

// Display fix (Display.cpp)
void InstallDisplayFix(HMODULE exe);
