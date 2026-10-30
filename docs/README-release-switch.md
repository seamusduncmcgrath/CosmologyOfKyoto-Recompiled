# Cosmology of Kyoto — recompiled for Nintendo Switch (homebrew)

This is a native Switch version of Cosmology of Kyoto (1993; English release 1995), made by
statically recompiling the game's Windows 3.1 program. It is not an emulator: the game's own
code runs directly, with a small layer that stands in for Windows 3.1, on top of SDL3
(devkitPro's Switch port). It runs as homebrew, from the Homebrew Menu.

You need the English Windows release of the game. Its files are not included: the movies,
pictures and sound in `CK_DATA`, and the program files it runs.

## Setting up

1. On the SD card, make a folder `/switch/CosmologyOfKyoto`.
2. Put `cok.nro` in it.
3. Copy everything from the Cosmology of Kyoto CD into the same folder, so that it holds
   `PLAY_E.EXE`, `FILEIO.DLL`, `CKFONT.FON` and the `CK_DATA` folder next to `cok.nro`.
4. Start it from the Homebrew Menu. It also runs from the album, since it uses well under
   the memory the album allows.

## Controls

| Input | Action |
|---|---|
| Left stick | Move the cursor (hold ZR to move faster) |
| D-pad | Move the cursor slowly, for fine aiming |
| A | Click |
| B | Right-click |
| Y | The keyboard. It also comes up by itself when the game asks for text |
| + | Quit: the game offers to save first. Press + again to quit at once, for screens that don't offer to save |

A USB keyboard and mouse work too.

## Saved games

The Switch has no file browser, so saved games are kept in a `Saves` folder next to
`cok.nro` and named with the keyboard (up to 8 letters and digits). To resume, type the name
of a saved game: the newest is filled in, and the others are listed above the keyboard.
Saved games are the same `.KPR` files as on Windows and macOS, so they can be copied between
them.

## Where things are kept

Everything is in the folder `cok.nro` is in:

| File | Contents |
|---|---|
| `Saves` | Your saved games |
| `settings.ini` | Settings |
| `WINDOWS` | Files the game writes to its `C:\WINDOWS` |
| `cok.log` | A log of the last run. Please include it if you report a problem. Started with `nxlink -s`, the log goes to the PC instead |

The program carries Wine's free re-creations of the Windows 3.1 system fonts (GNU LGPL 2.1,
see `COPYING.LIB` in the source).
