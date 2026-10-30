"""Make the program icons from the game's own COK_ICON.ICO (32x32, from the CD).

usage: python tools/make_icons.py [path/to/COK_ICON.ICO]     (default: in the repository root)

Writes generated/icons/: cok.ico (Windows: 16-256 px), cok.icns (macOS: 16-1024 px) and
switch.jpg (256x256 for the Switch homebrew menu). The icon is the game's, so like the
generated C it stays out of git (generated/ is ignored) but goes into source bundles. The
pixel art is enlarged with nearest-neighbour scaling, so it stays crisp. CMake uses these
when present; without them Windows and macOS get no custom icon and the Switch gets
platform/switch/icon.jpg.
"""
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def scaled(img, size):
    return img.resize((size, size), Image.NEAREST)


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "COK_ICON.ICO"
    if not src.is_file():
        raise SystemExit(f"error: {src} not found (it's on the game CD)")
    ico = Image.open(src)
    ico.size = max(ico.info["sizes"])
    img = ico.convert("RGBA")
    out = ROOT / "generated" / "icons"
    out.mkdir(parents=True, exist_ok=True)

    sizes = [16, 24, 32, 48, 64, 128, 256]
    big = scaled(img, 256)
    big.save(out / "cok.ico", sizes=[(s, s) for s in sizes],
             append_images=[scaled(img, s) for s in sizes])
    scaled(img, 1024).save(out / "cok.icns")
    sw = Image.new("RGB", (256, 256))                  # JPEG: transparency becomes black
    sw.paste(big, mask=big)
    sw.save(out / "switch.jpg", quality=95)
    print(f"wrote {out}: cok.ico, cok.icns, switch.jpg")


if __name__ == "__main__":
    main()
