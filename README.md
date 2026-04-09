# QuazzDoom

QuazzDoom is a Windows-focused modernization of the original `id-Software/DOOM` source release. This fork keeps the classic software renderer and gameplay intact while adding a practical Win32 build, controller support, fullscreen handling, sound/music playback, and a cleaner out-of-the-box experience on current Windows systems.

## What This Fork Adds

- Native Windows builds via CMake
- Console and GUI executables
- Win32 video backend with fullscreen support
- Mouse input and XInput controller support
- Windows sound effects and music playback
- Modernized defaults for the Windows build

## Build

### Windows x86

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"" -arch=x86 && cmake -S . -B build-win32 -G Ninja && cmake --build build-win32"
```

### Windows x64

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"" -arch=x64 && cmake -S . -B build -G Ninja && cmake --build build"
```

## Run

Place an IWAD such as `doom.wad` next to the built executable or pass it with `-iwad`.

GUI build:

```powershell
.\build-win32\bin\doom-game.exe -iwad doom.wad
```

Console build:

```powershell
.\build-win32\bin\doom.exe -iwad doom.wad
```

## Controls

### Keyboard

- `WASD` movement
- Mouse look-style turning
- `Space` use
- `Ctrl` fire
- `Shift` run

### Controller

- Left stick: move and strafe
- Right stick: turn
- `A`: use / confirm in menus
- `B`: back in menus
- `Y`: map
- `LB` / `RB`: previous / next weapon
- `LT`: hold to run
- `RT`: fire
- `Start`: menu

## Notes

- The 32-bit build remains the safest target for this codebase.
- Fullscreen starts enabled by default on the Win32 GUI build.
- `Alt+Enter` toggles fullscreen.

## License

The original source release remains subject to the upstream licensing terms included in this repository. See `LICENSE.TXT`.
