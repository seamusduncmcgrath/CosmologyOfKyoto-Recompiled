# Cosmology of Kyoto — recompiled for macOS

This is a native Mac version of Cosmology of Kyoto (1993; English release 1995), made by
statically recompiling the game's Windows 3.1 program. It is not an emulator: the game's own
code runs directly, with a small layer that stands in for Windows 3.1, on top of SDL3. It
runs on Apple Silicon and Intel Macs with macOS 10.15 or later.

You need the English Windows release of the game. Its files are not included: the movies,
pictures and sound in `CK_DATA`, and the program files the app runs.

## Setting up

1. Copy everything from the Cosmology of Kyoto CD into a folder. It should contain
   `PLAY_E.EXE`, `FILEIO.DLL`, `CKFONT.FON` and the `CK_DATA` folder.
2. Put **Cosmology of Kyoto.app** in that folder, or anywhere you like, such as
   Applications. If it can't find the game next to itself, it asks you to choose the folder
   the first time, and remembers it.
3. Open it. The app isn't notarized by Apple, so the first time, macOS will refuse to open
   it:
   - Control-click (or right-click) the app, choose **Open**, then **Open** again.
   - On recent macOS, you may instead need **System Settings › Privacy & Security › Open
     Anyway**.

## Controls

| Input | Action |
|---|---|
| Mouse / trackpad | Everything, as in the original |
| Option+Return / F11 | Switch between a window and fullscreen (remembered). The window's green button works too |
| Control+Q, Cmd+Q | Quit (the game offers to save first). The close button does the same. Asked a second time, it quits straight away, for screens that don't offer to save |
| Game controller | Stick moves the cursor, A clicks, B right-clicks, Start quits |

## Where things are kept

`~/Library/Application Support/CosmologyOfKyoto` holds:

| File | Contents |
|---|---|
| `Saves` | Your saved games. The save dialog starts here, but you can save anywhere |
| `settings.ini` | Fullscreen choice and the game folder |
| `WINDOWS/` | Files the game writes to its `C:\WINDOWS` |
| `cok.log` | A log of the last run. Please include it if you report a problem |

The game folder itself is never written to.

The `fonts` folder inside the app holds Wine's free re-creations of the Windows 3.1 system
fonts (GNU LGPL 2.1, see the `COPYING.LIB` file next to them).
