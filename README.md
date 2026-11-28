# Pinball Arcade - Modern Systems Fix

A fix that makes The Pinball Arcade (PC, Steam) play properly on modern computers. It is a
single DLL you drop into the game folder: no game files are edited, and deleting the DLL puts the
game back exactly as it was.

![A table in 16:9 with black bars on a 32:9 monitor](screenshots/table-fullscreen-32x9.jpg)

## What it fixes
- **Game speed in legacy windowed mode (DX11).** On a 120 Hz desktop, the unpatched game
  ran at a measured 119.84 fps in its original windowed mode and gameplay appeared too fast.
  The fix holds gameplay to the 60 fps its physics were made for. The speed problem was not
  reproduced in our native fullscreen test, where gameplay appeared normal-speed; fullscreen
  FPS was not measured. A monitor's refresh rate alone does not establish the game's speed.
- **Screen resolution.** In windowed mode the game opened as a small window in the corner of the
  screen that couldn't be moved. In DX11 native fullscreen it stretched a 2560x1440 picture
  across the whole 5120x1440 screen, distorting the image. (The old settings tool's DX11
  resolution list also stops at 4096 pixels wide.) The fix runs the game fullscreen on any
  monitor with the picture kept 16:9 and black bars at the sides (or top and bottom), or in a
  proper centred window. It renders at full resolution, so nothing is stretched, and stays sharp
  with Windows display scaling.
- **Broken settings tool.** The Launch Configuration Utility crashes on a fresh install. It is
  replaced by a settings window built into the fix, which opens when the game starts.
- **Stuck high-score box.** An original game bug: after entering initials on some tables
  (reproduced on Teed Off), the on-screen letter-select box stayed up into the next game and took
  over the flipper keys until the table was reloaded. The fix closes it.

Gameplay, physics, tables and graphics are otherwise unchanged.

## Install
1. Copy `bin\xinput1_3.dll` into the game folder, next to `PinballArcade.exe`. For Steam that is
   `<Steam library>\steamapps\common\PinballArcade\`.
   Optionally copy `bin\PinballArcadeFix.ini` too; it documents every setting, but the settings
   window creates it for you.
2. Start the game from Steam. Steam asks which version to run: pick the **DX11** option and tick
   Steam's box to remember the choice. From then on the game goes straight to the fix's settings
   window, and you never see the old Launch Configuration Utility again. You can still switch to
   DX9 in the settings window.

**Uninstall:** delete `xinput1_3.dll` (and `PinballArcadeFix.ini` / `PinballArcadeFix.log` if
present) from the game folder.

**Supported version:** the final Steam build (game version 1.71, Steam build 5650517, October
2020) of `PinballArcade11.exe` (DX11) and `PinballArcade.exe` (DX9). The fix checks the exe
before changing anything; on any other build it does nothing and says so in its log. The cabinet
version (`PinballArcadeCabinet.exe`) is not supported.

## Settings window
Opens when the game starts, before the game loads its settings, whichever version Steam starts:

![The settings window](screenshots/launcher-window.png)

- **Renderer**: DirectX 11 (recommended) or DirectX 9. If Steam started the other version, the
  fix restarts the game in the one chosen here.
- **Display**: *Fullscreen 16:9 with black bars* or *Windowed 16:9* (a normal window at 75% of
  the fullscreen size, 1920x1080 on a 2560x1440 or 5120x1440 screen, centred; it can be moved
  and minimised but not resized).
- **Graphics settings for the selected renderer**, with the same choices the old settings tool
  had on its DX11 and DX9 tabs. The list changes when you switch Renderer:
  - DirectX 11: **Anti-aliasing (MSAA)** (off, 4x, 8x), **MSAA quality** (0-100),
    **Anisotropic filtering** (off to 16x), **Ball brightening**.
  - DirectX 9: **Anti-aliasing** (off to 16x), **Texture filtering** (none, bilinear,
    anisotropic 4x to 16x), **Post-processing**, **Ball reflection**.
- **Language** and **Focus-sensitive Input** (whether the game ignores input while it isn't the
  active window), for both renderers.
- **Show this window every time the game starts**: untick to start straight away with your
  saved settings. Hold **Shift** while the game starts to bring the window back.
- **Play** starts the game; **Quit** cancels the launch.

All of these are written to the game's own settings files in the same format the old tool used.
The old tool's resolution and window-mode settings are replaced by **Display** (the fix uses your
monitor's current desktop resolution), and its Cabinet tab (screen layouts for multi-screen
arcade cabinets) is left out because the cabinet version isn't supported.

![The main menu in 16:9 on a 32:9 monitor](screenshots/menu-fullscreen-32x9.jpg)

## Settings file
Everything is stored in `PinballArcadeFix.ini` in the game folder. The settings window covers the
normal options; `bin\PinballArcadeFix.ini` lists them all with explanations, including a few
extras (`AspectRatio`, `FrameRateCap`). The game's own settings stay where the game keeps them,
in `Documents\..\My Games\Pinball Arcade\` (`settings.dat`, `dx11_settings.dat`).

## Troubleshooting
The fix writes `PinballArcadeFix.log` in the game folder each time the game starts: its version
(first line; also in `xinput1_3.dll`'s file properties), what it hooked, the window size it
chose, and a frame-timing line every 10 seconds. If something doesn't
work, that log shows what happened.

- **Steam's "Launch Configuration Utility" crashes.** That's the game's original tool (see below);
  use the fix's settings window instead.

## What was wrong, and what the fix does
`xinput1_3.dll` is loaded by the game at start-up (it's the controller library). The fix passes
every controller call through to the real Windows `xinput1_3.dll` and patches the game in memory.
All addresses below are in the supported build.

### Game speed in legacy windowed mode (DX11)
- `PinballArcade11.exe` advances the simulation by a fixed 16 ms for every frame it draws
  (frame timer at `0x49D7B0`), so it runs fast whenever it renders above 60 fps: about
  120 fps means about twice the intended speed.
- In its original **windowed** mode nothing caps that frame rate: the game presents with
  `Present(1)` (`0x73DB19`), once per monitor refresh, so on a 120 Hz desktop it renders at
  about 120 fps. Its present function (`0x73DA80`) does contain a frame limiter, a minimum of
  15 ms between frames (about 66 fps), but it only runs while the renderer's fullscreen flag is
  set (the flag the game's Alt+Enter toggle switches); in windowed mode that flag is off. If
  native fullscreen sets the same flag, that would explain why it looked normal-speed. This
  comes from reading the code and hasn't been confirmed by measuring fullscreen FPS. Desktop
  refresh rate alone doesn't prove the frame rate.
- The DX9 build has its own 60 fps busy-wait and doesn't have this problem.
- Confirmed reproduction: unpatched DX11 in legacy windowed mode on a 120 Hz desktop,
  with Terminator 2 appearing too fast. Native fullscreen appeared normal-speed in a separate
  test; its FPS was not measured, so we do not claim a fullscreen speed reproduction or
  that every fullscreen configuration is immune.
- The fix hooks the frame timer and holds each frame to 1/60 s with a high-resolution timer, and
  takes over the `Present` call to limit DXGI's frame queue to one frame, which keeps input lag
  low. Measured: a steady 16.67 ms per frame.

### Loading while capped (DX11)
- The game streams its data one chunk per frame, and the title screen only accepts input once its
  start-up loading is done. A plain 60 fps cap therefore makes loading slow: in an early version
  of this fix, the title ignored input for 20-45 seconds and tables took about 25 seconds to
  load. This was caused by the cap, not by the original game.
- So the cap is lifted while the game loads. During start-up, it stays off until disk reads stop
  and you press a key, button or controller button (20 second limit). For later loads, it is off
  while at least 12 of the last 16 frames read from disk, and back on after 8 frames without
  reads. Gameplay itself reads nothing from disk (measured), so a table in play is never sped up.
  With this, the title accepts input about a second after it appears and tables load in about
  3 seconds. Unpatched load times were not measured.

### Screen resolution (both builds)
- DX11 creates a normal window from its saved position and size (by default at 0,0 with the
  title bar off-screen) and sizes its swap chain to the window. DX9 opens 640x480 by default,
  picks its render size from a preset table and moves its own window.
- The fix hooks the game's `CreateWindowExA` and makes the game window either borderless at the
  largest 16:9 size that fits the monitor, with a black full-screen window behind it for the
  bars, or a normal centred window. The game renders at the window's size. For DX9 it also hooks
  the set-display-mode function (`0x5BA5A0`) to use that size, keeps the game out of exclusive
  fullscreen, pins `MoveWindow`, and updates the screen size the menus are laid out for.
- The process is marked per-monitor DPI aware, so Windows display scaling can't blur it.
- In our unpatched DX11 native fullscreen test, a 2560x1440 picture was stretched across the
  5120x1440 display. This display problem is separate from the speed problem reproduced in
  legacy windowed mode. The fix ignores the old tool's DX11 "Fullscreen" setting and blocks
  Alt+Enter, using its own borderless window instead. At the default 16:9 setting on this
  display, the patched game renders a centred 2560x1440 picture with black side bars;
  it does not render a 5120x1440 ultrawide game view.

### Settings tool
`PBAConfig.exe` (the Launch Configuration Utility) shows number fields for arcade-cabinet screen
layouts that are blank on a fresh install, and crashes reading them when you press Done. Its
DX11 resolution list also stops at 4096 wide. The fix's settings window replaces it: it runs at the
game's entry point, before the game reads its settings, and writes `settings.dat` and
`dx11_settings.dat` in the same formats `PBAConfig.exe` used (cabinet settings are left alone).

### Stuck high-score box
The game opens its letter-select box (GUI menu `0x23`) while a table's display shows that the
table wants initials. Only some table types contain the code that closes it again afterwards. If
the box is still open half a second after the game has finished with initials, the fix closes it
with the game's own "back to previous menu" function. Players reported this bug years ago, on
tables such as Cactus Jack's and Black Knight 2000.

## Verified
Tested on Windows 11 with a 5120x1440 120 Hz monitor:

- Unpatched DX11 legacy windowed mode, Terminator 2 (2026-11-27): PresentMon 2.6.0 measured
  1,436 frames over 11.974 seconds, averaging 119.84 fps / 8.34 ms per frame, with no dropped
  frames. Gameplay appeared too fast during the test. This measures frame rate; simulation
  speed was assessed by playing, not by a separate timing measurement.
- Unpatched DX11 native fullscreen: gameplay appeared normal-speed, with a 2560x1440 picture
  stretched across the 5120x1440 display. FPS was not measured in this test.
- Patched DX11 speed: steady 60 fps (16.67 ms frames), play-tested.
- Fullscreen and windowed, DX9 and DX11: correct 16:9 picture at native resolution (2560x1440
  fullscreen, 1920x1080 windowed), menus and tables play-tested.
- DX11 title screen accepts the first click or key about 1 second after it appears; tables load
  in about 3 seconds; play-tested.
- Settings window: writes settings both builds read (play-tested from Steam); Renderer switch
  tested both ways outside Steam.
- Stuck high-score box: reproduced on Teed Off (DX11) and closed by the fix; normal high-score
  entries are left alone. The DX9 version of this fix uses the same logic, with its addresses
  checked against the running game, but hasn't been seen closing a stuck box yet.

## Limitations
- **Cabinet mode is not supported.** The fix is for the regular PC version only, not
  `PinballArcadeCabinet.exe` or multi-screen arcade-cabinet setups (separate playfield, backglass
  and DMD screens). It does nothing in the cabinet exe, and the settings window has no Cabinet
  settings.
- Steam still lists the old Launch Configuration Utility. Removing it would mean changing the
  game's files; ticking Steam's "remember my choice" hides it.
- The fix uses the primary monitor, and reads the desktop resolution when the game starts.
- Online features that depended on FarSight's servers are not covered.

## Build
Run `build.cmd` (needs Visual Studio with "Desktop development with C++"; it builds a 32-bit DLL).
Output: `bin\xinput1_3.dll`.

## Files
- `src\` - the DLL source: `PinballFix.cpp` (entry, logging, speed/pacing/loading fixes, stuck
  high-score box), `Display.cpp` (fullscreen/windowed, DX9 resolution, focus), `Launcher.cpp`
  (settings window logic and game settings files), `LauncherUI.cpp` (reusable CerealCommitter
  launcher window), `proxy_xinput.cpp` + `xinput1_3.def` (pass-through to the real
  `xinput1_3.dll`), `resources.rc` (embedded logo).
- `bin\` - the built DLL and a commented settings file.
- `assets\` - the logo embedded in the DLL.
- `screenshots\` - the images in this README.
- `LICENSE` - MIT license.

## License
The code is released under the [MIT License](LICENSE): you can use, modify and share it, as long as
the copyright notice stays with it. It comes with no warranty.

Not covered by the license:
- The CerealCommitter logo (`assets\logo.png` and the copy built into `xinput1_3.dll`). It
  remains © CerealCommitter; if you reuse the code, use your own logo.
- The screenshots in `screenshots\`, which show the game. The Pinball Arcade is © FarSight
  Studios; the tables and their names are trademarks of their respective owners.
