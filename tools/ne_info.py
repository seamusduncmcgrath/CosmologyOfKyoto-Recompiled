"""Dump the structure of a Win16 NE executable: segments, relocations, imports, entry points."""
import struct
import sys
from collections import Counter, defaultdict


def main(path):
    data = open(path, "rb").read()
    ne = struct.unpack_from("<I", data, 0x3C)[0]
    if data[ne:ne + 2] != b"NE":
        sys.exit("not an NE file")

    (linkver, linkrev, ent_off, ent_len, crc, flags, autodata, heap, stack,
     csip, sssp, nseg, nmod, nresname_len, seg_off, res_off, resname_off,
     modref_off, impname_off, nresname_off, nmovent, align, nres, exetype) = \
        struct.unpack_from("<BBHHIHHHHIIHHHHHHHHIHHHB", data, ne + 2)

    shift = 1 << (align or 9)
    print(f"{path}: linker {linkver}.{linkrev}, flags {flags:#06x}, exetype {exetype}")
    print(f"entry CS:IP {csip >> 16}:{csip & 0xFFFF:#06x}  SS:SP {sssp >> 16}:{sssp & 0xFFFF:#06x}  "
          f"autodata seg {autodata}, heap {heap:#x}, stack {stack:#x}")

    def pstr(off):
        n = data[off]
        return data[off + 1:off + 1 + n].decode("latin-1")

    modrefs = [struct.unpack_from("<H", data, ne + modref_off + 2 * i)[0] for i in range(nmod)]
    modules = [pstr(ne + impname_off + m) for m in modrefs]

    imports = defaultdict(Counter)  # module -> ordinal/name -> fixup count
    reltypes = Counter()
    code_total = data_total = 0

    print(f"\n{nseg} segments:")
    for i in range(nseg):
        sector, length, sflags, minalloc = struct.unpack_from("<HHHH", data, ne + seg_off + 8 * i)
        length = length or 0x10000
        is_data = sflags & 1
        kind = "DATA" if is_data else "CODE"
        if is_data:
            data_total += length
        else:
            code_total += length
        nrel = 0
        if sflags & 0x100 and sector:
            roff = sector * shift + length
            nrel = struct.unpack_from("<H", data, roff)[0]
            for r in range(nrel):
                atype, rtype, a, b, c = struct.unpack_from("<BBHHH", data, roff + 2 + 8 * r)
                target = rtype & 3
                reltypes[("internal", "imp-ord", "imp-name", "osfixup")[target]] += 1
                if target == 1:
                    imports[modules[b - 1]][f"#{c}"] += 1
                elif target == 2:
                    imports[modules[b - 1]][pstr(ne + impname_off + c)] += 1
        print(f"  seg {i + 1:3}: {kind} len {length:#07x} flags {sflags:#06x} relocs {nrel}")

    print(f"\ncode total {code_total:,} bytes, data total {data_total:,} bytes")
    print("relocation kinds:", dict(reltypes))

    print(f"\nimported modules ({len(modules)}): {', '.join(modules)}")
    for mod, funcs in sorted(imports.items()):
        print(f"  {mod}: {len(funcs)} distinct imports")
        print("    " + " ".join(sorted(funcs, key=lambda s: (s[0] != '#', int(s[1:]) if s[0] == '#' else s))))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
        print("\n" + "=" * 70 + "\n")
