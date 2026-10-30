"""Draw the Switch homebrew icon (platform/switch/icon.jpg, 256x256): an original design, not
game art, so it can live in the repository.

usage: python tools/switch_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SIZE = 256


def font(names, size):
    for name in names:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    img = Image.new("RGB", (SIZE, SIZE))
    d = ImageDraw.Draw(img)
    for y in range(SIZE):                                   # night sky, indigo to black
        t = y / (SIZE - 1)
        d.line([(0, y), (SIZE, y)], fill=(int(28 * (1 - t)), int(24 * (1 - t)), int(64 * (1 - t) + 8)))
    glow = Image.new("L", (SIZE, SIZE))                     # a vermilion moon with a soft glow
    ImageDraw.Draw(glow).ellipse((68, 30, 188, 150), fill=255)
    glow = glow.filter(ImageFilter.GaussianBlur(14))
    img.paste((150, 40, 30), mask=glow.point(lambda v: v // 2))
    d.ellipse((78, 40, 178, 140), fill=(214, 64, 40))
    serif = font(["georgia.ttf", "times.ttf", "DejaVuSerif.ttf"], 30)
    small = font(["georgia.ttf", "times.ttf", "DejaVuSerif.ttf"], 22)
    for text, f, y in (("Cosmology", serif, 168), ("of Kyoto", small, 204)):
        w = d.textlength(text, font=f)
        d.text(((SIZE - w) / 2, y), text, font=f, fill=(236, 228, 210))
    out = ROOT / "platform" / "switch" / "icon.jpg"
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, quality=92)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
