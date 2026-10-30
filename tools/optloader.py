"""Decompressor for SLR Systems OPTLOADER (1993) compressed Win16 NE segments.

This is a direct port of the LZ routine at 1:0031 in PLAY_E.EXE. The bit stream is
16-bit little-endian control words consumed MSB-first, interleaved with literal and
offset bytes in a single byte stream. A new control word is fetched *as soon as* the
16th bit of the previous one has been consumed (before any following data byte), which
mirrors the `add bp,bp / dec dx / je reload` sequence in the original.

Token grammar (after each control bit):
    1                          literal byte
    0 0 b                      length 2+b        (2..3)
    0 1 0 b                    length 4+b        (4..5)
    0 1 1 0 b                  length 6+b        (6..7)
    0 1 1 1 0 bb               length 8+bb       (8..11)
    0 1 1 1 1 0 bbb            length 12+bbb     (12..19)
    0 1 1 1 1 1 <byte n>       n < 0x81: length n; n == 0x81: no-op; n > 0x81: end
For length 2 the distance high byte is 0; otherwise it is coded as:
    0 0                        0
    0 1 0                      1
    0 1 1 b                    2+b
    1 0 0 bb                   4+bb
    1 0 1 bbb                  8+bbb
    1 1 0 bbbb                 16+bbbb
    1 1 1 0 bbbb               32+bbbb
    1 1 1 1 0 bbbb             48+bbbb
    1 1 1 1 1 bbbbbb           64+bbbbbb
followed by the low byte from the stream; distance = ((high << 8) | low) + 1.
"""


class _Bits:
    def __init__(self, src, pos):
        self.src = src
        self.pos = pos
        self.word = self.read16()
        self.left = 16

    def read8(self):
        b = self.src[self.pos]
        self.pos += 1
        return b

    def read16(self):
        w = self.src[self.pos] | (self.src[self.pos + 1] << 8)
        self.pos += 2
        return w

    def bit(self):
        b = (self.word >> 15) & 1
        self.word = (self.word << 1) & 0xFFFF
        self.left -= 1
        if self.left == 0:
            self.word = self.read16()
            self.left = 16
        return b

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v


def _high_byte(s):
    if not s.bit():
        if not s.bit():
            return 0
        if not s.bit():
            return 1
        return 2 + s.bits(1)
    if not s.bit():
        if not s.bit():
            return 4 + s.bits(2)
        return 8 + s.bits(3)
    if not s.bit():
        return 16 + s.bits(4)
    if not s.bit():
        return 32 + s.bits(4)
    if not s.bit():
        return 48 + s.bits(4)
    return 64 + s.bits(6)


def decompress(src, pos=0, out=None):
    """Decompress from src[pos:] until the end marker.

    Returns (output bytearray, position just past the compressed data). If `out` is
    given, output is appended to it, so matches may reach back into existing contents
    (the loader decompresses in place after a prefix of uncompressed bytes).
    """
    out = bytearray() if out is None else out
    s = _Bits(src, pos)
    while True:
        if s.bit():
            out.append(s.read8())
            continue
        if not s.bit():
            length = 2 + s.bit()
        elif not s.bit():
            length = 4 + s.bit()
        elif not s.bit():
            length = 6 + s.bit()
        elif not s.bit():
            length = 8 + s.bits(2)
        elif not s.bit():
            length = 12 + s.bits(3)
        else:
            n = s.read8()
            if n == 0x81:
                continue
            if n > 0x81:
                return out, s.pos
            length = n
        high = 0 if length == 2 else _high_byte(s)
        dist = ((high << 8) | s.read8()) + 1
        if dist > len(out):
            raise ValueError(f"match distance {dist} beyond output start ({len(out)} bytes) at src {s.pos:#x}")
        start = len(out) - dist
        for i in range(length):  # byte-by-byte: overlapping copies are intentional
            out.append(out[start + i])
