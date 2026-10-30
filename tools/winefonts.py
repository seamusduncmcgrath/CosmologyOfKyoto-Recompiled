"""Make the Windows 3.1 system bitmap fonts for platforms that don't have them (macOS,
Linux, consoles) from Wine's free re-creations (LGPL 2.1).

usage: python tools/winefonts.py OUTDIR

Downloads Wine 5.0's bitmap-only TrueType fonts (the last release that kept the built .ttf
files in its source tree) and writes one Windows FNT 3.0 file per bitmap size, which
runtime/src/gdi_text.c loads like a FONT resource. Glyphs come from Wine; the face metrics
(point size, internal leading, default character, family) are those of the Windows fonts
of the same pixel size, so font matching and text layout come out as on Windows. Only the
sizes Windows has are kept.
"""
import io
import struct
import sys
import urllib.request
from pathlib import Path

from fontTools.ttLib import TTFont

WINE = "https://raw.githubusercontent.com/wine-mirror/wine/wine-5.0/"

# (Wine file, face, height) -> metrics of the Windows 3.1 font of that size (vgasys.fon,
# vgafix.fon, sserife.fon, smalle.fon, coure.fon): points, internal leading, weight,
# pitch and family byte, default char, average width.
FACES = [
    ("system", "System", 16, dict(points=10, il=3, weight=700, pf=0x21, default=0x80, avg=7)),
    ("fixedsys", "Fixedsys", 15, dict(points=12, il=3, weight=400, pf=0x30, default=0x80, avg=8)),
    ("ms_sans_serif", "MS Sans Serif", 13, dict(points=8, il=2, weight=400, pf=0x21, default=0x81, avg=5)),
    ("ms_sans_serif", "MS Sans Serif", 16, dict(points=10, il=3, weight=400, pf=0x21, default=0x81, avg=7)),
    ("ms_sans_serif", "MS Sans Serif", 20, dict(points=12, il=4, weight=400, pf=0x21, default=0x81, avg=8)),
    ("small_fonts", "Small Fonts", 11, dict(points=7, il=2, weight=400, pf=0x21, default=0x80, avg=5)),
    ("courier", "Courier", 13, dict(points=10, il=0, weight=400, pf=0x30, default=0x80, avg=8)),
]
FIRST, LAST = 0x20, 0xFF


def strike_glyphs(font, height):
    """{glyph name: (width, bearingX, bearingY, rows as lists of 0/1)} and the strike ascent."""
    eblc, ebdt = font["EBLC"], font["EBDT"]
    for si, strike in enumerate(eblc.strikes):
        bst = strike.bitmapSizeTable
        if bst.hori.ascender - bst.hori.descender != height:
            continue
        metrics = {}                                  # format 5 keeps metrics in the index
        for sub in strike.indexSubTables:
            m = getattr(sub, "metrics", None)
            if m is not None:
                for name in sub.names:
                    metrics[name] = m
        glyphs = {}
        for name, bm in ebdt.strikeData[si].items():
            m = getattr(bm, "metrics", None) or metrics.get(name)
            if m is None:
                continue
            w, h = m.width, m.height
            rows = []
            for r in range(h):
                data = bm.getRow(r, bitDepth=1, metrics=m)
                rows.append([(data[x >> 3] >> (7 - (x & 7))) & 1 for x in range(w)])
            bx = getattr(m, "BearingX", getattr(m, "horiBearingX", 0))
            by = getattr(m, "BearingY", getattr(m, "horiBearingY", 0))
            adv = getattr(m, "Advance", getattr(m, "horiAdvance", w))
            glyphs[name] = (adv, bx, by, rows)
        return glyphs, bst.hori.ascender
    raise SystemExit(f"no {height}px strike")


def make_fnt(font, face, height, mt):
    glyphs, ascent = strike_glyphs(font, height)
    cmap = font.getBestCmap()
    def glyph_for(code):
        try:
            u = ord(bytes([code]).decode("cp1252"))
        except UnicodeDecodeError:
            return None
        name = cmap.get(u)
        return glyphs.get(name) if name else None
    default = glyph_for(mt["default"]) or glyph_for(ord("?"))
    chars = []
    for code in range(FIRST, LAST + 1):
        g = glyph_for(code) or default
        adv, bx, by, rows = g
        cell = [[0] * adv for _ in range(height)]     # advance x height cell, origin at the baseline
        for r, row in enumerate(rows):
            y = ascent - by + r
            for c, bit in enumerate(row):
                x = bx + c
                if bit and 0 <= x < adv and 0 <= y < height:
                    cell[y][x] = 1
        chars.append((adv, cell))
    fixed = len({adv for adv, _ in chars}) == 1
    n = LAST - FIRST + 1
    header_size, table_size = 148, (n + 1) * 6
    bitmaps = io.BytesIO()
    table = []
    for adv, cell in chars:                           # column-major, 8 pixels per byte column
        table.append((adv, header_size + table_size + bitmaps.tell()))
        for col in range((adv + 7) // 8):
            for y in range(height):
                b = 0
                for k in range(8):
                    x = col * 8 + k
                    if x < adv and cell[y][x]:
                        b |= 0x80 >> k
                bitmaps.write(bytes([b]))
    sentinel = header_size + table_size + bitmaps.tell()
    table.append((0, sentinel))
    face_off = sentinel
    total = face_off + len(face) + 1
    maxw = max(adv for adv, _ in chars)
    h = bytearray(header_size)
    struct.pack_into("<HI", h, 0, 0x0300, total)
    h[6:66] = b"Wine bitmap font (LGPL 2.1); metrics as Windows 3.1".ljust(60, b"\0")[:60]
    struct.pack_into("<HHHHHHH", h, 66, 0, mt["points"], 96, 96, ascent, mt["il"], 0)
    struct.pack_into("<BBBHB", h, 80, 0, 0, 0, mt["weight"], 0)
    struct.pack_into("<HHBHH", h, 86, chars[0][0] if fixed else 0, height, mt["pf"], mt["avg"], maxw)
    struct.pack_into("<BBBBH", h, 95, FIRST, LAST, mt["default"] - FIRST, 0, 0)
    struct.pack_into("<IIII", h, 101, 0, face_off, 0, header_size + table_size)
    struct.pack_into("<I", h, 118, (0x01 if fixed else 0x02) | 0x10)        # DFF_FIXED/PROPORTIONAL, 1COLOR
    out = bytes(h) + b"".join(struct.pack("<HI", w, o) for w, o in table) + bitmaps.getvalue()
    return out + face.encode("latin-1") + b"\0"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    cache = {}
    for file, face, height, mt in FACES:
        if file not in cache:
            data = urllib.request.urlopen(f"{WINE}fonts/{file}.ttf").read()
            cache[file] = TTFont(io.BytesIO(data))
        fnt = make_fnt(cache[file], face, height, mt)
        name = f"{face.replace(' ', '').upper()[:6]}{height:02d}.FNT"
        (out / name).write_bytes(fnt)
        print(f"{name}: {face} {height}px, {len(fnt):,} bytes")
    (out / "COPYING.LIB").write_bytes(urllib.request.urlopen(WINE + "COPYING.LIB").read())
    (out / "README.txt").write_text(
        "Bitmap fonts for platforms without the Windows 3.1 system fonts: Wine's free\n"
        "re-creations of System, Fixedsys, MS Sans Serif, Small Fonts and Courier, converted\n"
        "to Windows FNT files by tools/winefonts.py. They come from Wine 5.0\n"
        "(https://gitlab.winehq.org/wine/wine, fonts/*.ttf, built from fonts/*.sfd) and are\n"
        "distributed under the GNU Lesser General Public License 2.1 (COPYING.LIB).\n", newline="\n")


if __name__ == "__main__":
    main()
