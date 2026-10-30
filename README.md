# Cosmology of Kyoto — Static Recompilation

A native port of **Cosmology of Kyoto** (Softedge, 1993; English release 1995) for modern
Windows, macOS and the Nintendo Switch, made by *statically recompiling* the game's original 16-bit
Windows 3.1 program into C.

This is not an emulator. There is no CPU interpreter and no copy of Windows: the game's own
16-bit x86 code has been translated, function by function, into C that compiles to native
code for each platform. A small runtime stands in for the parts of Windows 3.1 the game uses,
built on [SDL3](https://libsdl.org).

## Status

The game runs on every platform below, with sound, voice subtitles, saving and loading, and
the in-game Reference. Bug reports are welcome: please include `cok.log`.

| Platform | Build | Notes |
|---|---|---|
| Windows 10/11 (x64) | `cok.exe`, a single file | Drop it into the game folder and run it |
| macOS 10.15+ | `Cosmology of Kyoto.app` | Universal: Apple Silicon and Intel |
| Nintendo Switch | `cok.nro` (homebrew) | Joy-Con cursor, system keyboard |

What's added on top of the original:

- **Window or fullscreen** (Alt+Enter / F11 on Windows, Option+Return / F11 on macOS). The
  640×480 picture is scaled sharply: whole-number scales are pixel-exact, and other sizes
  use "sharp bilinear" so pixels stay crisp without uneven widths.
- **Game controllers**: the stick moves the cursor and A clicks (on the Switch, and with any
  controller on a computer).
- **Native file dialogs** for saving and loading. Saved games go to a per-user folder, and
  the game folder is never written to.
- **No setup**: no installer, no Windows 3.1, no compatibility settings. The original
  `PLAY_E.EXE` is unpacked in memory at start-up.

## Playing

Each release comes with a README covering setup and controls:

- Windows: [docs/README-release.md](docs/README-release.md)
- macOS: [docs/README-release-macos.md](docs/README-release-macos.md)
- Switch: [docs/README-release-switch.md](docs/README-release-switch.md)

In short:

1. Copy everything from the game files into a folder.
2. Put the program in that folder:
   - Windows: `cok.exe`.
   - macOS: the app. It can also live elsewhere; it asks for the game folder once.
   - Switch: `cok.nro`, with the folder at `/switch/CosmologyOfKyoto/` on the SD card.
3. Run it.

## How it works

Cosmology of Kyoto is a Macromedia Director 4.0 title. The game itself is Lingo bytecode
inside the `.DXR` files; `PLAY_E.EXE` is Director's "projector", the 16-bit Windows program
that interprets it. The projector is what gets recompiled. With it, Director's own Lingo
interpreter, renderer and sound mixer run natively, so the game behaves exactly as it did.

1. **Unpack.** `PLAY_E.EXE` is compressed with SLR Systems' OPTLOADER, a 1993
   self-loading packer. `tools/unpack.py` documents and reverses the format, and
   `runtime/src/optloader.c` does the same at start-up, so players use their original file.
2. **Analyse.** IDA Pro, with the passes in `tools/ida/analyze.py`, recovers every function,
   jump table and call target. `tools/ida/export.py` writes the result as JSON.
3. **Recompile.** `tools/recompile.py` (Python, using Capstone) turns each function into C:
   - **CPU state:** registers live in a struct, and flags are computed only where something
     reads them.
   - **Memory:** 16-bit segmented memory is emulated through a selector table, so guest
     pointers are never host pointers. That makes 64-bit and ARM hosts straightforward.
   - **Calls:** these become direct C calls, and indirect calls go through a dispatch table.

   The instruction translation is checked against [Unicorn](https://www.unicorn-engine.org)
   on random inputs for every instruction form the game uses. Three modules are recompiled:
   - the projector;
   - `FILEIO.DLL`, Director's file XObject;
   - `MACROMIX.DLL`, Director's software sound mixer, which the projector carries inside it.
4. **Run.** The runtime (`runtime/`, C) implements the Windows 3.1 APIs the game calls on top
   of SDL3:
   - **KERNEL:** memory, selectors, files, modules and resources;
   - **USER:** windows, messages, input, dialogs and controls;
   - **GDI:** an 8-bit palettized screen, bitmaps, DIBs, regions and raster fonts;
   - **MMSYSTEM:** waveOut and multimedia timers;
   - **COMMDLG:** the file dialogs;
   - plus the DOS calls the C runtime makes.

   Interrupt-time sound callbacks are delivered at safe points between API calls.

The generated C is about 680,000 lines for the projector and compiles cleanly with clang and
GCC. The same generated code runs unchanged on x86-64 and ARM64.

## Building from source

The generated C is translated from the game's own program, so it isn't in this repository
and isn't distributed. To build, you need:

- the game files;
- **IDA Pro**, for the analysis step;
- **Python 3**, with `pip install -r requirements.txt` (Capstone, plus Unicorn for tests);
- **CMake 3.20+**. SDL3 is downloaded and built automatically, and linked statically. The
  compilers are:
  - Windows: clang and Ninja (those bundled with Visual Studio work);
  - macOS: the Xcode command line tools;
  - Switch: devkitPro's devkitA64, libnx and switch-mesa.

### 1. Unpack, analyse, generate C

```sh
python tools/unpack.py -v PLAY_E.EXE build/PLAY_E.unpacked.exe
```

Open `build/PLAY_E.unpacked.exe` and `FILEIO.DLL` in IDA and run in each:

```python
exec(open(r"<repo>/tools/ida/analyze.py").read()); analyze(); validate()
exec(open(r"<repo>/tools/ida/export.py").read()); export(r"<repo>/build/PLAY_E.analysis.json")  # or FILEIO
```

`MACROMIX.DLL` is embedded in the projector, which writes it to its `C:\WINDOWS` at
start-up. You can take it from the per-user `WINDOWS` folder after one run of any build, or
extract it from the projector. Then analyse it in IDA the same way. Then:

```sh
python tools/recompile.py emit build/PLAY_E.unpacked.exe build/PLAY_E.analysis.json --module play_e --out generated/play_e
python tools/recompile.py emit FILEIO.DLL build/FILEIO.analysis.json --module fileio --out generated/fileio
python tools/recompile.py emit build/MACROMIX.DLL build/MACROMIX.analysis.json --module macromix --out generated/macromix
python tools/make_icons.py      # optional: the game's own icon, from COK_ICON.ICO
```

`python tools/recompile.py test ...` runs the differential tests against Unicorn.

### 2. Build

**Windows:**

```sh
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/cmake
build/cmake/cok.exe --game "C:/path/to/the/game"
```

**macOS:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
open "build/Cosmology of Kyoto.app"
```

**Nintendo Switch.** Run this in devkitPro's MSYS2 shell on Windows, or with devkitPro on
Linux:

```sh
$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake -S . -B build/switch -DCMAKE_BUILD_TYPE=Release
make -C build/switch -j6 cok_nro
```

This uses devkitPro's [SDL3 fork](https://github.com/devkitPro/SDL) (branch
`switch-sdl-3.4`). It isn't packaged, so CMake downloads a pinned commit.

`python tools/package.py` makes the player release zip on Windows and macOS. To build on a
Mac from a Windows checkout, `python tools/source_bundle.py` packs the tracked files plus
your generated C. Keep that bundle to yourself, as it contains the translated game code.

### Debugging aids

- `-v` logs every API call.
- `COK_CLICK="ms,x,y;ms,'text'"` scripts input.
- `COK_FILE_DIALOG=path` answers file dialogs without asking.
- `COK_SHOT=file%d.bmp` saves screenshots.
- `COK_WAVE_DUMP=file.raw` records the sound output.

Without a console, the log goes to `cok.log` in the per-user folder.

## Legal

Cosmology of Kyoto is © Softedge (1993–1995). This project contains no game data
and no game code. The program only works with files from an original copy, which it checks
at start-up. The recompiled C is produced locally from your own copy and is never committed
or distributed in source form.

Third-party components:

- **SDL3** (zlib licence), and devkitPro's Switch fork of it.
- **The bundled bitmap fonts** in `fonts/`, converted from [Wine](https://www.winehq.org)'s
  re-creations of the Windows 3.1 system fonts: GNU LGPL 2.1, see
  [fonts/COPYING.LIB](fonts/COPYING.LIB).
- **Capstone and Unicorn**, used by the build tools only.

## Thanks

- Softedge, for a strange and wonderful game.
- The Wine project, whose documentation of Win16 behaviour made the runtime possible.
- The SDL and devkitPro developers.
