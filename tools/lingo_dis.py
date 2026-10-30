"""Disassemble compiled Lingo (Director 4 'Lscr' chunks) from a RIFX movie, for debugging.

usage: lingo_dis.py MOVIE.DXR [cast member number ...]

Lists each script's handlers with their bytecode, names resolved through the movie's
'Lnam' chunk. Big-endian (Mac-authored) RIFX only, which is what this game's movies are.
Opcode set as documented by ProjectorRays / ScummVM's Lingo bytecode notes.
"""
import struct
import sys

OPS0 = {
    0x01: "ret", 0x02: "retFactory", 0x03: "pushZero", 0x04: "mul", 0x05: "add", 0x06: "sub",
    0x07: "div", 0x08: "mod", 0x09: "inv", 0x0a: "joinStr", 0x0b: "joinPadStr", 0x0c: "lt",
    0x0d: "ltEq", 0x0e: "ntEq", 0x0f: "eq", 0x10: "gt", 0x11: "gtEq", 0x12: "and", 0x13: "or",
    0x14: "not", 0x15: "containsStr", 0x16: "contains0Str", 0x17: "getChunk", 0x18: "hiliteChunk",
    0x19: "ontoSpr", 0x1a: "intoSpr", 0x1b: "getField", 0x1c: "startTell", 0x1d: "endTell",
    0x1e: "pushList", 0x1f: "pushPropList", 0x21: "swap",
}
OPS1 = {
    0x01: "pushInt8", 0x02: "pushArgListNoRet", 0x03: "pushArgList", 0x04: "pushCons", 0x05: "pushSymb",
    0x06: "pushVarRef", 0x08: "getGlobal2", 0x09: "getGlobal", 0x0a: "getProp", 0x0b: "getParam",
    0x0c: "getLocal", 0x0e: "setGlobal2", 0x0f: "setGlobal", 0x10: "setProp", 0x11: "setParam",
    0x12: "setLocal", 0x13: "jmp", 0x14: "endRepeat", 0x15: "jmpIfZ", 0x16: "localCall", 0x17: "extCall",
    0x18: "objCallV4", 0x19: "put", 0x1a: "putChunk", 0x1b: "deleteChunk", 0x1c: "get", 0x1d: "set",
    0x1f: "getMovieProp", 0x20: "setMovieProp", 0x21: "getObjProp", 0x22: "setObjProp", 0x23: "tellCall",
    0x24: "peek", 0x25: "pop", 0x26: "theBuiltin", 0x27: "objCall", 0x2d: "pushChunkVarRef",
    0x2e: "pushInt16", 0x2f: "pushInt32", 0x30: "getChainedProp", 0x31: "pushFloat32",
    0x32: "getTopLevelProp", 0x33: "newObj",
}
NAMED = {"pushSymb", "getGlobal", "getGlobal2", "setGlobal", "setGlobal2", "getProp", "setProp", "extCall",
         "objCall", "objCallV4", "getObjProp", "setObjProp", "getMovieProp", "setMovieProp", "getChainedProp",
         "getTopLevelProp", "theBuiltin", "pushVarRef", "tellCall", "newObj"}


def chunks(d):
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    mo = u32(24)
    hdr_len, entry_len = struct.unpack_from(">HH", d, mo + 8)
    n = u32(mo + 16)
    for i in range(n):
        e = mo + 8 + hdr_len + i * entry_len
        yield i, d[e:e + 4], u32(e + 8), u32(e + 4)


def names(d):
    for _, tag, off, size in chunks(d):
        if tag == b"Lnam":
            b = d[off + 8:off + 8 + size]
            p = struct.unpack_from(">H", b, 16)[0]
            out = []
            while p < len(b):
                n = b[p]
                out.append(b[p + 1:p + 1 + n].decode("latin1"))
                p += 1 + n
            return out
    return []


def disassemble(b, nm, cast):
    g = lambda fmt, o: struct.unpack_from(fmt, b, o)[0]
    hcount, hoff = g(">H", 72), g(">I", 74)
    lcount, loff = g(">H", 78), g(">I", 80)
    ldoff = g(">I", 88)
    lits = []
    for i in range(lcount):
        t, o = struct.unpack_from(">HI", b, loff + 6 * i)
        if t == 1:
            n = g(">I", ldoff + o)
            lits.append(repr(b[ldoff + o + 4:ldoff + o + 4 + n].rstrip(b"\0").decode("latin1")))
        elif t == 4:
            lits.append(str(o))
        else:
            lits.append(f"<lit type {t}>")
    name = lambda i: nm[i] if 0 <= i < len(nm) else f"name#{i}"
    print(f"== script for cast member {cast}: {hcount} handlers, literals {lits}")
    for h in range(hcount):
        r = hoff + 42 * h                      # D4 handler records are 42 bytes
        nid, clen, coff = g(">h", r), g(">I", r + 4), g(">I", r + 8)
        print(f"on {name(nid)}")
        p, end = coff, coff + clen
        while p < end:
            op = b[p]
            at = p - coff
            if op < 0x40:
                txt, p = OPS0.get(op, f"op{op:02x}"), p + 1
            else:
                base = op & 0x3F
                if op >= 0x80:
                    arg, p = g(">H", p + 1), p + 3
                else:
                    arg, p = b[p + 1], p + 2
                mn = OPS1.get(base, f"op{op:02x}")
                if mn in NAMED:
                    txt = f"{mn} {name(arg)}"
                elif mn == "pushCons":
                    txt = f"pushCons {lits[arg // 6] if arg // 6 < len(lits) else arg}"
                elif mn in ("jmp", "jmpIfZ"):
                    txt = f"{mn} -> {at + arg}"
                elif mn == "endRepeat":
                    txt = f"{mn} -> {at - arg}"
                else:
                    txt = f"{mn} {arg}"
            print(f"   {at:4d}  {txt}")


def main():
    d = open(sys.argv[1], "rb").read()
    want = {int(x) for x in sys.argv[2:]}
    nm = names(d)
    for _, tag, off, size in chunks(d):
        if tag != b"Lscr":
            continue
        b = d[off + 8:off + 8 + size]
        cast = struct.unpack_from(">H", b, 46)[0]
        if not want or cast in want:
            disassemble(b, nm, cast)


if __name__ == "__main__":
    main()
