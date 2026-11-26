#pragma once
// Reusable CerealCommitter launcher window ("option B": logo panel on the left, settings on
// the right). Game-specific code only supplies the texts and the choices.
#define IDR_LAUNCHER_LOGO 101

#ifndef RC_INVOKED
#include <windows.h>

#include <string>
#include <vector>

struct LauncherChoice {
    std::wstring label;               // e.g. "Resolution"
    std::vector<std::wstring> items;  // drop-down entries
    int selected = 0;                 // in/out
};

struct LauncherSpec {
    std::wstring windowTitle;   // title bar
    std::wstring gameTitle;     // big heading, e.g. L"ABZ\x00DB"
    std::wstring subtitle;      // e.g. L"Ultrawide & Super-Ultrawide Fix"
    std::vector<LauncherChoice> choices;
    std::wstring checkboxText;
    std::wstring hintText;      // small text under the checkbox
    bool checked = true;        // in/out
};

// Shows the window modally on the calling thread. Logo is read from IDR_LAUNCHER_LOGO in
// resModule. Returns true if the player pressed Play (spec.choices[i].selected and
// spec.checked are updated), false for Quit / close.
bool ShowLauncher(HMODULE resModule, LauncherSpec& spec);
#endif
