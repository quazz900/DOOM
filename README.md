# QuazzDoom

QuazzDoom is a Windows-focused fork of the original `id-Software/DOOM` source release. It keeps the classic software-rendered game intact, but updates the port so it builds and runs cleanly on current Windows systems with a native Win32 game executable, working audio, mouse input, and XInput controller support.

## Status

- Windows-only codebase
- Single GUI executable: `doom.exe`
- Native Win32 video, input, sound, and music backends
- Software-rendered classic DOOM gameplay

## What This Fork Adds

- CMake-based Windows build
- GUI-only `doom.exe` with no console window
- Fullscreen enabled by default, with `Alt+Enter` toggle
- Mouse input and XInput controller support
- Windows sound effects and MIDI music playback
- Windows-friendly defaults and config handling
- Episode 4 support when the loaded IWAD provides it

## Build

QuazzDoom is currently maintained as a 32-bit Windows build. That remains the safest target for this codebase.

### Windows x86

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"" -arch=x86 && cmake -S . -B build-win32 -G Ninja && cmake --build build-win32"
```

The built executable is:

```text
build-win32\bin\doom.exe
```

## Run

Place an IWAD such as `doom.wad` next to `doom.exe`, or launch with `-iwad`.

Example:

```powershell
.\build-win32\bin\doom.exe -iwad doom.wad
```

On Windows, the game also looks for its config in:

```text
%USERPROFILE%\doom-win32.cfg
```

## Controls

### Keyboard and Mouse

- `W`, `A`, `S`, `D`: move / strafe
- Mouse: turn
- `Space`: use
- `Ctrl`: fire
- `Shift`: run
- `Esc`: menu
- `Alt+Enter`: toggle fullscreen

### XInput Controller

- Left stick: move and strafe
- Right stick: turn
- D-pad: menu navigation
- `A`: use, confirm, menu enter
- `B`: menu back
- `Y`: automap
- `LB` / `RB`: previous / next weapon
- `LT`: hold to run
- `RT`: fire
- `Start`: menu

## Notes

- QuazzDoom is based on the original DOOM source release and still expects a valid IWAD.
- The repository includes the source code only, not commercial game data.
- Fullscreen starts enabled by default on the Windows build.

## License

The original source release remains subject to the upstream licensing terms included in this repository. See `LICENSE.TXT`.
