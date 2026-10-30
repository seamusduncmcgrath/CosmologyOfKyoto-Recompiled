"""Unpack an SLR Systems OPTLOADER self-loading Win16 NE executable into a plain NE file.

OPTLOADER (shipped with OPTLINK, 1993) marks the module self-loading (NE flag 0x0800).
Segment 1 holds the loader: an 'A0' self-loader table, a small LZ decompressor and a
compressed tail containing BootApp/LoadAppSeg. Every other segment is stored as

    [fixup count:u16][LZ stream (see optloader.py)][packed fixup records]

Packed fixup records are `[type:u8][count:u8]` followed by a payload:

    type bits 0-1   address kind: 0 lobyte, 1 selector, 2 far pointer, 3 offset
    type bit 2      additive
    type bits 3-4   target: 0 internal, 1 import ordinal, 2 import name, 3 OS fixup
    internal        seg:u8, then count x (location:u16, offset:u16)   seg 0xFF = movable,
                    in which case the second word is an entry-table ordinal
    import          module:u16, then count x (location:u16, ordinal-or-name-offset:u16)
    OS fixup        fixup type:u16 (low byte used), then count x location:u16
    type 0xF0       non-additive selector chain to an internal segment:
                    count x (seg:u8, location:u16)

The output keeps every byte of the original file at its original offset (resources and
the Director projector data use absolute file offsets), appends the decompressed
segments with standard NE relocation tables, repoints the segment table at them, clears
the self-loading flag, and re-appends the original trailer so end-of-file lookups of the
Director `MFWV` trailer still work.
"""
import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

from optloader import decompress

NE_FLAG_SELFLOAD = 0x0800
SEG_DATA = 0x0001
SEG_PACKED = 0x0008        # NE "iterated" bit; OPTLOADER sets it on compressed segments
SEG_RELOCINFO = 0x0100

ADDR_KIND_TO_NE = {0: 0, 1: 2, 2: 3, 3: 5}   # lobyte, selector, far ptr, offset
NE_ADDR_NAMES = {0: "lobyte", 2: "selector", 3: "farptr", 5: "offset"}
TARGET_NAMES = {0: "internal", 1: "import-ordinal", 2: "import-name", 3: "osfixup"}
TRAILER_LEN = 8            # "MFWV" + u32 absolute offset of the PJ93 projector header


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


class NEFile:
    def __init__(self, data):
        self.data = data
        self.ne = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.ne:self.ne + 2] != b"NE":
            raise ValueError("not an NE executable")
        ne = self.ne
        self.flags = u16(data, ne + 0x0C)
        self.nseg = u16(data, ne + 0x1C)
        self.nmod = u16(data, ne + 0x1E)
        self.seg_table = ne + u16(data, ne + 0x22)
        self.modref_table = ne + u16(data, ne + 0x28)
        self.impname_table = ne + u16(data, ne + 0x2A)
        self.align = u16(data, ne + 0x32) or 9

    def segment(self, n):
        """Return (sector, disk_len, flags, minalloc) for 1-based segment n."""
        return struct.unpack_from("<HHHH", self.data, self.seg_table + 8 * (n - 1))

    def module_name(self, idx):
        off = self.impname_table + u16(self.data, self.modref_table + 2 * (idx - 1))
        return self.data[off + 1:off + 1 + self.data[off]].decode("latin-1")

    def import_name(self, name_off):
        off = self.impname_table + name_off
        return self.data[off + 1:off + 1 + self.data[off]].decode("latin-1")


def parse_fixups(buf, pos, count):
    """Parse packed fixup records. Returns (list of NE reloc tuples, end position).

    Each tuple is (ne_addr_type, ne_reloc_type, location, target1, target2).
    """
    relocs = []
    remaining = count
    while remaining:
        typ, n = buf[pos], buf[pos + 1]
        pos += 2
        remaining -= n
        if n == 0 or remaining < 0:
            raise ValueError(f"bad fixup record count {n} (type {typ:#04x}) at {pos - 2:#x}")
        if typ == 0xF0:
            for _ in range(n):
                relocs.append((2, 0, u16(buf, pos + 1), buf[pos], 0))
                pos += 3
            continue
        if typ & 0xE0:
            raise ValueError(f"unknown fixup record type {typ:#04x} at {pos - 2:#x}")
        addr = ADDR_KIND_TO_NE[typ & 3]
        target = (typ >> 3) & 3
        rtype = target | (4 if typ & 4 else 0)
        if target == 0:
            seg = buf[pos]
            pos += 1
            for _ in range(n):
                relocs.append((addr, rtype, u16(buf, pos), seg, u16(buf, pos + 2)))
                pos += 4
        elif target in (1, 2):
            mod = u16(buf, pos)
            pos += 2
            for _ in range(n):
                relocs.append((addr, rtype, u16(buf, pos), mod, u16(buf, pos + 2)))
                pos += 4
        else:
            fixtype = u16(buf, pos) & 0xFF
            pos += 2
            for _ in range(n):
                relocs.append((addr, rtype, u16(buf, pos), fixtype, 0))
                pos += 2
    return relocs, pos


def check_relocs(seg_no, image, relocs):
    """Walk non-additive chains to make sure every fixup site is inside the segment."""
    sites = 0
    for addr, rtype, loc, _, _ in relocs:
        width = {0: 1, 2: 2, 3: 4, 5: 2}[addr]
        chained = addr != 0 and not (rtype & 4) and (rtype & 3) != 3
        seen = set()
        while True:
            if loc + width > len(image):
                raise ValueError(f"seg {seg_no}: fixup at {loc:#x} outside segment ({len(image):#x})")
            if loc in seen:
                raise ValueError(f"seg {seg_no}: fixup chain loops at {loc:#x}")
            seen.add(loc)
            sites += 1
            if not chained:
                break
            loc = u16(image, loc)
            if loc == 0xFFFF:
                break
    return sites


def unpack_loader_segment(ne):
    """Decompress segment 1 the way BootApp does; returns (image, raw reloc table bytes)."""
    sector, length, flags, minalloc = ne.segment(1)
    start = sector << ne.align
    seg = ne.data[start:start + length]
    if seg[:2] != b"A0":
        raise ValueError("segment 1 does not start with an 'A0' self-loader table")
    # BootApp: the tail at 0x1E7 (length word at 0x2E) is decompressed in place.
    tail_len = u16(seg, 0x2E)
    if 0x1E7 + tail_len != length:
        raise ValueError(f"unexpected loader layout (tail {tail_len:#x}, segment {length:#x})")
    image, end = decompress(seg, 0x1E7, bytearray(seg[:0x1E7]))
    if end != length:
        raise ValueError(f"loader tail did not end at segment end ({end:#x} != {length:#x})")
    if b"OPTLOADER" not in image or b"SLR Systems" not in image:
        raise ValueError("segment 1 is not an SLR Systems OPTLOADER")
    if not flags & SEG_RELOCINFO:
        return image, b""
    nrel = u16(ne.data, start + length)
    return image, ne.data[start + length:start + length + 2 + 8 * nrel]


def unpack(data, verbose=False):
    ne = NEFile(data)
    if not ne.flags & NE_FLAG_SELFLOAD:
        raise ValueError("module is not self-loading; nothing to unpack")

    loader_image, loader_relocs = unpack_loader_segment(ne)
    segments = {1: (loader_image, loader_relocs)}
    stats = Counter()
    imports = Counter()
    report = [f"seg   1: loader      disk {ne.segment(1)[1]:#07x} -> {len(loader_image):#07x}"
              f"  minalloc {ne.segment(1)[3]:#07x}  ({len(loader_relocs) // 8} relocs, kept)"]

    for n in range(2, ne.nseg + 1):
        sector, length, flags, minalloc = ne.segment(n)
        minalloc = minalloc or 0x10000
        if sector == 0:
            segments[n] = (bytearray(), b"")
            report.append(f"seg {n:3}: no file data, minalloc {minalloc:#07x}")
            continue
        start = sector << ne.align
        disk_len = length or 0x10000
        count = u16(data, start)
        image, pos = decompress(data, start + 2)
        if len(image) > minalloc:
            raise ValueError(f"seg {n}: decompressed {len(image):#x} bytes > minalloc {minalloc:#x}")
        relocs, pos = parse_fixups(data, pos, count)
        if pos > start + disk_len:
            raise ValueError(f"seg {n}: packed data overruns segment ({pos - start:#x} > {disk_len:#x})")
        sites = check_relocs(n, image, relocs)
        for addr, rtype, _, t1, t2 in relocs:
            stats[(NE_ADDR_NAMES[addr], TARGET_NAMES[rtype & 3], bool(rtype & 4))] += 1
            if rtype & 3 == 1:
                imports[(ne.module_name(t1), f"#{t2}")] += 1
            elif rtype & 3 == 2:
                imports[(ne.module_name(t1), ne.import_name(t2))] += 1
        table = struct.pack("<H", len(relocs)) + b"".join(struct.pack("<BBHHH", *r) for r in relocs)
        segments[n] = (image, table if relocs else b"")
        kind = "DATA" if flags & SEG_DATA else "CODE"
        report.append(f"seg {n:3}: {kind} disk {disk_len:#07x} -> {len(image):#07x}  minalloc {minalloc:#07x}"
                      f"  {len(relocs):4} relocs ({sites} sites)  slack {start + disk_len - pos}")

    out = bytearray(data)
    struct.pack_into("<H", out, ne.ne + 0x0C, ne.flags & ~NE_FLAG_SELFLOAD)
    unit = 1 << ne.align
    for n in range(1, ne.nseg + 1):
        sector, length, flags, minalloc = ne.segment(n)
        image, table = segments[n]
        if not image:
            continue
        out += bytes(-len(out) % unit)
        new_sector = len(out) >> ne.align
        if new_sector > 0xFFFF:
            raise ValueError("output too large for the alignment shift")
        out += image + table
        flags &= ~SEG_PACKED
        flags = flags | SEG_RELOCINFO if table else flags & ~SEG_RELOCINFO
        struct.pack_into("<HHHH", out, ne.seg_table + 8 * (n - 1),
                         new_sector, len(image) & 0xFFFF, flags, minalloc)
    out += data[-TRAILER_LEN:]

    if verbose:
        print("\n".join(report))
        print("\nrelocations by kind:")
        for (addr, target, additive), c in sorted(stats.items()):
            print(f"  {target:15} {addr:9} {'additive' if additive else '':9} {c}")
        print(f"\n{len(imports)} distinct imports:")
        by_mod = {}
        for (mod, name), c in imports.items():
            by_mod.setdefault(mod, []).append((name, c))
        for mod in sorted(by_mod):
            names = sorted(by_mod[mod], key=lambda x: (x[0][0] != "#", int(x[0][1:]) if x[0][0] == "#" else x[0]))
            print(f"  {mod} ({len(names)}): " + " ".join(name for name, _ in names))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    try:
        result = unpack(args.input.read_bytes(), args.verbose)
    except ValueError as e:
        sys.exit(f"error: {e}")
    args.output.write_bytes(result)
    print(f"wrote {args.output} ({len(result):,} bytes)")


if __name__ == "__main__":
    main()
