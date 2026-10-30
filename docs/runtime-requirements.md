# Runtime requirements

What the recompiled `PLAY_E.EXE` (Director 4.0 projector) and `FILEIO.DLL` need from the
runtime, beyond executing their code. Findings from Phase 2 (IDA analysis of the unpacked
binaries); see `docs/imports.md` for the full static import list with reference counts.

## Static imports (358 functions, all by ordinal)

| Module | Functions | Reference sites | Notes |
|---|---:|---:|---|
| USER | 168 | 1158 | windows, messages, dialogs, menus, cursors |
| KERNEL | 70 | 1095 | memory (Global*/Local*), files, resources, profile strings; `__AHSHIFT`/`__AHINCR` constants (464 sites) |
| GDI | 106 | 872 | DCs, bitmaps/DIBs, palettes, text, regions |
| KEYBOARD | 4 | 8 | `ToAscii`, `OemToAnsi`, `AnsiToOemBuff`, `OemToAnsiBuff` |
| MMSYSTEM | 3 | 8 | `timeGetTime`, `mciSendString`, `mciGetErrorString` |
| SHELL | 4 | 7 | drag & drop, `FindExecutable` |
| WIN87EM | 1 | 6 | `__fpMath` (FPU emulator control; a real FPU is assumed) |
| COMMDLG | 3 | 5 | `GetOpenFileName`, `GetSaveFileName`, `PrintDlg` |

`AllocCStoDSAlias` is imported but only used by the OPTLOADER stub; there is no evidence of
self-modifying code in the program itself.

## APIs resolved at runtime (`LoadLibrary` + `GetProcAddress`, 53 call sites)

| Library | Requested | Status on the original disc |
|---|---|---|
| `MACROMIX.DLL` | 26 `wave_*` functions (`wave_SessionCreate`, `wave_BufferCreate/Play`, `wave_ChannelOpen/SetVolume`, `wave_Pump`, ...) | **not shipped** (only referenced as a string), so Director uses a fallback sound path |
| `mmsystem.dll` | `mciSendCommand`, `mciSendString`, `mciGetErrorString` | system |
| `msvideo.dll` | `VideoForWindowsVersion` | system (VfW) |
| `wing.dll` | `WinGCreateDC`, `WinGRecommendDIBFormat`, `WinGCreateBitmap`, `WinGGetDIBPointer`, `WinGSetDIBColorTable`, `WinGBitBlt` | optional; `PLAY_E.INI` has `Wing=0` |
| `QTIM.DLL`, `QTIMCMGR.DLL` | QuickTime Component Manager (`_EntryPoint`, `_CMgrInitialize`, `_CMgrTerminate`, ordinals 6/7/11/63/64) | optional (QuickTime for Windows) |
| `toolhelp.dll` | `TimerCount`, `MemManInfo`, `LocalFirst`, `LocalNext` | system |
| KERNEL (by name) | `THHOOK` | KERNEL internals; return NULL / fake |
| printer driver | `DeviceCapabilities` | printing only |

`GetProcAddress` must therefore return callable thunks for names as well as ordinals. A
missing library has to fail the way it did on a 1995 machine, so Director takes the same
fallback paths.

## Direct system calls

- **DOS `int 21h`** (35 sites, plus 22 calls through KERNEL `DOS3Call`): get date/time
  (2Ah/2Ch), version (30h), disk free (36h), chdir (3Bh), close (3Eh), write (40h),
  delete (41h), seek (42h), getcwd (47h), exit (4Ch), find first/next (4Eh/4Fh) with DTA
  (1Ah), rename (56h), commit (68h), disk reset (0Dh), drives (0Eh/19h), interrupt vectors
  (25h/35h), get attributes (4300h). One generic `__intdosx`.
- **DPMI `int 31h`**: 0604h (page size), 0500h (free memory info).
- **Video BIOS `int 10h`**: 1007h/1015h (read VGA palette/DAC registers).

## CPU features

- x87 floating point as real FPU instructions (1,084 OS-fixup sites only matter without an
  FPU, and are ignored).
- 386 instructions appear (32-bit registers with `66h` prefixes, `push large`, `enter`).
- Huge pointers: selector arithmetic with `__AHSHIFT` = 3 / `__AHINCR` = 8.

## Known oddities

- `cseg100:0x34f4` (`sub_BD854`, a huge-pointer fill routine reached from
  `cseg67:0x28dc` when its `arg_14 != 0`): every branch lands 2 bytes before the
  apparently intended target, including one into the middle of an instruction. As
  shipped the routine looks broken, and is probably never executed. The recompiler must
  support overlapping instruction streams (it decodes from each target itself).
- The C runtime's hand-written floating-point code (cseg02) has 109 jumps between
  functions and one call into the middle of a routine. The recompiler groups such
  functions into multi-entry regions.
