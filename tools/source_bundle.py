"""Pack the source for building on another machine (e.g. a Mac): every file git tracks plus
the generated C, which git ignores and which needs the game's binaries to recreate.

usage: python tools/source_bundle.py

Writes build/dist/cok-src-<git version>.zip. On the other machine: unzip, then
`python3 tools/package.py` (or configure and build with CMake directly). No game files are
included: git doesn't track them. The generated C is translated from the game's programs, so
treat the bundle like the game itself and keep it to your own machines.
"""
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    version = subprocess.run(["git", "describe", "--always", "--dirty"], capture_output=True, text=True,
                             cwd=ROOT).stdout.strip() or "dev"
    tracked = subprocess.run(["git", "ls-files", "-z"], capture_output=True, text=True, check=True,
                             cwd=ROOT).stdout.split("\0")
    files = [ROOT / f for f in tracked if f and (ROOT / f).is_file()]
    generated = sorted((ROOT / "generated").rglob("*"))
    if not any(p.suffix == ".c" for p in generated):
        raise SystemExit("error: no generated C; run tools/recompile.py emit first (see CMakeLists.txt)")
    files += [p for p in generated if p.is_file()]
    name = f"cok-src-{version}"
    out = ROOT / "build" / "dist" / f"{name}.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in files:
            z.write(f, f"{name}/{f.relative_to(ROOT).as_posix()}")
    print(f"wrote {out} ({len(files)} files, {out.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
