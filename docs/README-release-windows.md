# Cosmology of Kyoto — recompiled for modern Windows

This is a native Windows version of Cosmology of Kyoto (1993; English release 1995), made by
statically recompiling the game's Windows 3.1 program. It is not an emulator: the game's own
code runs directly, with a small layer that stands in for Windows 3.1, on top of SDL3.

You need the English Windows release of the game. Its files are not included: the movies,
pictures and sound in `CK_DATA`, and the program files `cok.exe` runs.

## Setting up

1. Copy everything from the Cosmology of Kyoto CD into a folder, for example
   `C:\Games\Cosmology of Kyoto`. The folder should contain `PLAY_E.EXE`, `FILEIO.DLL`,
   `CKFONT.FON` and the `CK_DATA` folder.
2. Put `cok.exe` in that same folder.
3. Run `cok.exe`.

No installation, Windows 3.1, or compatibility settings are needed.

## Controls

| Input | Action |
|---|---|
| Mouse | Everything, as in the original |
| Alt+Enter / F11 | Switch between a window and fullscreen (remembered for next time) |
| Ctrl+Q | Quit (the game offers to save first) |
| Close button | The same as Ctrl+Q. Click it again to quit straight away, on screens that don't offer to save |
| Game controller | Stick moves the cursor, A clicks, B right-clicks, Start quits |

## Where things are kept

Your saved games go to the `Saves` folder in `%APPDATA%\CosmologyOfKyoto`. The save dialog
starts there, but you can save anywhere you like. The game folder itself is never written to,
so it can be read-only.

`%APPDATA%\CosmologyOfKyoto` also holds:

| File | Contents |
|---|---|
| `settings.ini` | Your window/fullscreen choice |
| `WINDOWS\` | Files the game writes to its `C:\WINDOWS` (preferences, sound driver) |
| `cok.log` | A log of the last run. If something goes wrong, please include it in your report |

## Command line (optional)

```
cok.exe [--fullscreen | --windowed] [--game FOLDER]
```

| Option | Effect |
|---|---|
| `--game FOLDER` | Run the game from another folder instead of the one `cok.exe` is in |
| `--fullscreen` | Start fullscreen this time |
| `--windowed` | Start in a window this time |
