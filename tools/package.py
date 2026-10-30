"""Build a release and pack it for players in build/dist/<name>.zip.

Windows: cok.exe and README.md. Players copy the game's files (the CD contents) to a
folder, drop cok.exe in, and run it; nothing else is needed (C runtime and SDL linked in).
macOS: "Cosmology of Kyoto.app" (universal: Apple Silicon and Intel, ad-hoc signed) and
README.md. The app finds the game folder next to itself or asks for it once.

usage: python tools/package.py [--name NAME]

Needs the generated C (tools/recompile.py emit, see CMakeLists.txt, or a source bundle made
by tools/source_bundle.py) and CMake; on Windows clang and Ninja, on macOS the Xcode command
line tools. CMake is taken from $CMAKE, the PATH, or Visual Studio's bundled copy.
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "release"
DIST = ROOT / "build" / "dist"
VS_CMAKE = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions"
                r"\Microsoft\CMake\CMake\bin\cmake.exe")
VS_LLVM = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin")
# DLLs every supported Windows (10 and later) has; anything else would need shipping.
SYSTEM_DLLS = {"advapi32", "gdi32", "imm32", "kernel32", "oleaut32", "setupapi", "shell32", "user32",
               "version", "winmm", "dbghelp", "ole32", "cfgmgr32", "hid", "dwmapi", "dinput8", "dxgi",
               "d3d11", "d3d12", "uxtheme", "ntdll", "comdlg32", "shlwapi", "combase"}


def find_cmake():
    for c in (os.environ.get("CMAKE"), shutil.which("cmake"), str(VS_CMAKE)):
        if c and Path(c).exists():
            return c
    sys.exit("error: cmake not found (set CMAKE)")


def run(*args):
    print("+", " ".join(str(a) for a in args))
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT)


def check_imports(exe):
    """Fail if cok.exe imports a DLL that isn't part of Windows (e.g. VCRUNTIME140.dll)."""
    objdump = shutil.which("llvm-objdump") or str(VS_LLVM / "llvm-objdump.exe")
    if not Path(objdump).exists():
        print("note: llvm-objdump not found; DLL imports not checked")
        return
    out = subprocess.run([objdump, "-p", str(exe)], capture_output=True, text=True, check=True).stdout
    dlls = sorted({l.split(":", 1)[1].strip() for l in out.splitlines() if "DLL Name:" in l})
    extra = [d for d in dlls if d.lower().removesuffix(".dll") not in SYSTEM_DLLS
             and not d.lower().startswith("api-ms-win-")]
    print("imports:", ", ".join(dlls))
    if extra:
        sys.exit(f"error: cok.exe needs DLLs that Windows doesn't have: {', '.join(extra)}")


def git_version():
    try:
        return subprocess.run(["git", "describe", "--always", "--dirty"], capture_output=True, text=True,
                              check=True, cwd=ROOT).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", help="package name (default: CosmologyOfKyoto-Recomp-<git version>)")
    args = ap.parse_args()
    name = args.name or f"CosmologyOfKyoto-Recomp-{git_version()}"

    cmake = find_cmake()
    mac = sys.platform == "darwin"
    if mac:
        name += "-macOS"
    gen = ["-G", "Ninja"] if shutil.which("ninja") or not mac else []
    cc = [] if mac else ["-DCMAKE_C_COMPILER=clang"]
    run(cmake, "-S", ".", "-B", BUILD, *gen, *cc,
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG", "-DCOK_RELEASE=ON")
    run(cmake, "--build", BUILD, "--target", "cok", "--parallel")

    out = DIST / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    zip_path = DIST / f"{name}.zip"
    if mac:
        app = BUILD / "Cosmology of Kyoto.app"
        run("lipo", "-archs", app / "Contents" / "MacOS" / "Cosmology of Kyoto")
        shutil.copytree(app, out / app.name, symlinks=True)
        run("codesign", "--force", "--deep", "--sign", "-", out / app.name)      # ad-hoc
        shutil.copy2(ROOT / "docs" / "README-release-macos.md", out / "README.md")
        if zip_path.exists():
            zip_path.unlink()
        run("ditto", "-c", "-k", "--keepParent", out, zip_path)
    else:
        exe = BUILD / "cok.exe"
        check_imports(exe)
        shutil.copy2(exe, out / "cok.exe")
        shutil.copy2(ROOT / "docs" / "README-release-windows.md", out / "README.md")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(out.iterdir()):
                z.write(f, f"{name}/{f.name}")
    print(f"wrote {zip_path} ({zip_path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
