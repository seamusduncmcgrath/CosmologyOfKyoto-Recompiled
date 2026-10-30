"""Win16 NE module loader: segments, relocation fixups (per patched byte range), entry table."""
import struct
from dataclasses import dataclass, field

# NE relocation address types (what gets patched) and widths in bytes
LOBYTE, SELECTOR, FARPTR, OFFSET = 0, 2, 3, 5
ADDR_WIDTH = {LOBYTE: 1, SELECTOR: 2, FARPTR: 4, OFFSET: 2}

SEG_DATA = 0x0001
SEG_RELOCINFO = 0x0100


@dataclass(frozen=True)
class Internal:
    """A location inside this module: segment number and offset (None for a bare selector)."""
    seg: int
    off: int | None


@dataclass(frozen=True)
class Import:
    module: str
    ordinal: int | None = None
    name: str | None = None

    def __str__(self):
        return f"{self.module}.{self.name or '#%d' % self.ordinal}"


@dataclass(frozen=True)
class OsFixup:
    kind: int


@dataclass(frozen=True)
class Fixup:
    """One patched site: `width` bytes at `site` receive `kind` of `target`."""
    site: int
    kind: int            # LOBYTE / SELECTOR / FARPTR / OFFSET
    target: object       # Internal / Import / OsFixup
    additive: bool

    @property
    def width(self):
        return ADDR_WIDTH[self.kind]


@dataclass
class Segment:
    num: int
    data: bytes          # file image zero-filled to minalloc, unrelocated (chain links in place)
    flags: int
    minalloc: int
    fixups: dict = field(default_factory=dict)   # site offset -> Fixup

    @property
    def is_code(self):
        return not self.flags & SEG_DATA

    def fixups_in(self, start, end):
        """Fixups whose patched bytes overlap [start, end)."""
        return [f for site in range(max(0, start - 3), end) if (f := self.fixups.get(site))
                and site + f.width > start]


def _u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


class NEModule:
    def __init__(self, path):
        self.path = str(path)
        data = open(path, "rb").read()
        self.raw = data
        ne = struct.unpack_from("<I", data, 0x3C)[0]
        if data[ne:ne + 2] != b"NE":
            raise ValueError(f"{path}: not an NE executable")
        h = lambda off: _u16(data, ne + off)
        self.flags = h(0x0C)
        self.autodata = h(0x0E)
        self.entry_cs_ip = (h(0x16), h(0x14))
        self.ss_sp = (h(0x1A), h(0x18))
        nseg, nmod = h(0x1C), h(0x1E)
        seg_table, modref, impname = ne + h(0x22), ne + h(0x28), ne + h(0x2A)
        entry_table, entry_len = ne + h(0x04), h(0x06)
        resname = ne + h(0x26)
        align = h(0x32) or 9

        def pstr(off):
            return data[off + 1:off + 1 + data[off]].decode("latin-1")

        self.name = pstr(resname)
        self.modules = [pstr(impname + _u16(data, modref + 2 * i)) for i in range(nmod)]
        self.entries, self.entry_flags = self._parse_entries(data, entry_table, entry_len)

        self.segments = {}
        for i in range(nseg):
            sector, length, flags, minalloc = struct.unpack_from("<HHHH", data, seg_table + 8 * i)
            start = sector << align
            length = (length or 0x10000) if sector else 0
            minalloc = minalloc or 0x10000
            # the loader zero-fills up to minalloc; code can rely on it (OPTLOADER even drops
            # a trailing zero byte, e.g. the high byte of a segment's final `retf N`)
            image = data[start:start + length].ljust(minalloc, b"\0")
            seg = Segment(i + 1, image, flags, minalloc)
            if flags & SEG_RELOCINFO and sector:
                rel = start + length     # relocation records follow the on-disk image
                for r in range(_u16(data, rel)):
                    self._add_reloc(seg, struct.unpack_from("<BBHHH", data, rel + 2 + 8 * r), impname, pstr)
            self.segments[i + 1] = seg

    def patched_prologs(self):
        """Exported entries whose `push ds / pop ax / nop` or `mov ax, ds / nop` prologue the
        Windows loader rewrites to `mov ax, DGROUP`: every exported function of a library
        with a single data segment (Wine: NE_FixupSegmentPrologs). Programs call DLL exports
        directly, so this is how such a function finds its own data. {(seg, off): length}"""
        out = {}
        if not (self.flags & 0x8000 and self.flags & 0x0001 and self.autodata):
            return out
        for ordinal, (seg, off) in self.entries.items():
            s = self.segments.get(seg)
            if not (self.entry_flags.get(ordinal, 0) & 1) or not s or not s.is_code:
                continue
            head = bytes(s.data[off:off + 3])
            if head in (bytes([0x1E, 0x58, 0x90]), bytes([0x8C, 0xD8, 0x90])):   # push ds; pop ax; nop | mov ax, ds; nop
                out[(seg, off)] = 3
        return out

    @staticmethod
    def _parse_entries(data, off, length):
        """Entry table -> {ordinal: (seg, offset)}, {ordinal: flags} (bit 0 exported, bit 1
        shared data)."""
        entries, flags, ordinal, end = {}, {}, 1, off + length
        while off < end:
            count, kind = data[off], data[off + 1] if off + 1 < end else 0
            off += 2
            if count == 0:
                break
            if kind == 0:                      # unused ordinals
                ordinal += count
                continue
            for _ in range(count):
                flags[ordinal] = data[off]
                if kind == 0xFF:               # movable: flags, int 3Fh, seg, offset
                    seg, eoff = data[off + 3], _u16(data, off + 4)
                    off += 6
                else:                          # fixed: flags, offset
                    seg, eoff = kind, _u16(data, off + 1)
                    off += 3
                entries[ordinal] = (seg, eoff)
                ordinal += 1
        return entries, flags

    def _add_reloc(self, seg, rec, impname, pstr):
        atype, rtype, loc, t1, t2 = rec
        target_kind, additive = rtype & 3, bool(rtype & 4)
        if target_kind == 0:
            if t1 == 0xFF:
                target = Internal(*self.entries[t2])
            else:
                target = Internal(t1, t2 if atype != SELECTOR else None)
        elif target_kind == 1:
            target = Import(self.modules[t1 - 1], ordinal=t2)
        elif target_kind == 2:
            target = Import(self.modules[t1 - 1], name=pstr(impname + t2))
        else:
            target = OsFixup(t1)
        chained = not additive and atype != LOBYTE and target_kind != 3
        while True:
            seg.fixups[loc] = Fixup(loc, atype, target, additive)
            if not chained:
                break
            loc = _u16(seg.data, loc)
            if loc == 0xFFFF:
                break
