/* SLR Systems OPTLOADER unpacker: turns the self-loading, compressed PLAY_E.EXE into the
 * plain NE image the recompiled code was generated from, in memory at start-up.
 *
 * A C port of tools/unpack.py and tools/optloader.py (see there for the formats); the output
 * is byte-for-byte what the Python tool writes, so the loader's image check covers both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define NE_FLAG_SELFLOAD 0x0800
#define SEG_DATA 0x0001
#define SEG_PACKED 0x0008
#define SEG_RELOCINFO 0x0100
#define TRAILER_LEN 8

typedef struct Buf {
    uint8_t *p;
    size_t n, cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra <= b->cap)
        return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p)
        return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int buf_put(Buf *b, const void *data, size_t n)
{
    if (!buf_reserve(b, n))
        return 0;
    memcpy(b->p + b->n, data, n);
    b->n += n;
    return 1;
}

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- the LZ decompressor (optloader.py) --------------------------------------------------- */

typedef struct Bits {
    const uint8_t *src;
    size_t pos, end;
    unsigned word, left;
    int bad;
} Bits;

static unsigned bits_rd8(Bits *s)
{
    if (s->pos >= s->end) {
        s->bad = 1;
        return 0;
    }
    return s->src[s->pos++];
}

static unsigned bits_rd16(Bits *s)
{
    unsigned lo = bits_rd8(s);
    return lo | bits_rd8(s) << 8;
}

static unsigned bit(Bits *s)
{
    unsigned b = (s->word >> 15) & 1;
    s->word = (s->word << 1) & 0xFFFF;
    if (--s->left == 0) {
        s->word = bits_rd16(s);
        s->left = 16;
    }
    return b;
}

static unsigned bitsn(Bits *s, int n)
{
    unsigned v = 0;
    while (n--)
        v = v << 1 | bit(s);
    return v;
}

static unsigned high_byte(Bits *s)
{
    if (!bit(s)) {
        if (!bit(s))
            return 0;
        if (!bit(s))
            return 1;
        return 2 + bitsn(s, 1);
    }
    if (!bit(s)) {
        if (!bit(s))
            return 4 + bitsn(s, 2);
        return 8 + bitsn(s, 3);
    }
    if (!bit(s))
        return 16 + bitsn(s, 4);
    if (!bit(s))
        return 32 + bitsn(s, 4);
    if (!bit(s))
        return 48 + bitsn(s, 4);
    return 64 + bitsn(s, 6);
}

/* Decompress src[pos..end) onto `out` (matches may reach into what's already there).
 * Returns the position just past the compressed data, or 0 on error. */
static size_t decompress(const uint8_t *src, size_t pos, size_t end, Buf *out)
{
    Bits s = { src, pos, end, 0, 16, 0 };
    s.word = bits_rd16(&s);
    for (;;) {
        if (s.bad)
            return 0;
        if (bit(&s)) {
            uint8_t v = (uint8_t)bits_rd8(&s);
            if (!buf_put(out, &v, 1))
                return 0;
            continue;
        }
        unsigned length;
        if (!bit(&s))
            length = 2 + bit(&s);
        else if (!bit(&s))
            length = 4 + bit(&s);
        else if (!bit(&s))
            length = 6 + bit(&s);
        else if (!bit(&s))
            length = 8 + bitsn(&s, 2);
        else if (!bit(&s))
            length = 12 + bitsn(&s, 3);
        else {
            unsigned n = bits_rd8(&s);
            if (n == 0x81)
                continue;
            if (n > 0x81)
                return s.bad ? 0 : s.pos;
            length = n;
        }
        unsigned high = length == 2 ? 0 : high_byte(&s);
        size_t dist = ((size_t)high << 8 | bits_rd8(&s)) + 1;
        if (s.bad || dist > out->n || !buf_reserve(out, length))
            return 0;
        size_t start = out->n - dist;
        for (unsigned i = 0; i < length; i++)        /* overlapping copies are intentional */
            out->p[out->n++] = out->p[start + i];
    }
}

/* ---- packed fixups (unpack.py parse_fixups) -> a standard NE relocation table ------------ */

static const uint8_t ADDR_KIND_TO_NE[4] = { 0, 2, 3, 5 };     /* lobyte, selector, far ptr, offset */

static int put_rec(Buf *t, unsigned addr, unsigned rtype, unsigned loc, unsigned t1, unsigned t2)
{
    uint8_t rec[8] = { (uint8_t)addr, (uint8_t)rtype };
    wr16le(rec + 2, (uint16_t)loc);
    wr16le(rec + 4, (uint16_t)t1);
    wr16le(rec + 6, (uint16_t)t2);
    return buf_put(t, rec, 8);
}

/* Records are [type][n] then a payload: 0xF0 = n x (seg:u8, loc:u16) selector chains;
 * otherwise a header (internal: seg:u8, import: module:u16, OS fixup: type:u16) and n
 * entries (internal/import: loc:u16, offset-or-ordinal:u16; OS fixup: loc:u16). Returns
 * the position after them, or 0. */
static size_t parse_fixups(const uint8_t *d, size_t pos, size_t end, unsigned count, Buf *table)
{
    uint8_t zero[2] = { 0, 0 };
    buf_put(table, zero, 2);                         /* entry count, filled in at the end */
    unsigned total = 0;
    int remaining = (int)count;
#define NEED(k) do { if (pos + (k) > end) return 0; } while (0)
    while (remaining > 0) {
        NEED(2);
        unsigned typ = d[pos], n = d[pos + 1];
        pos += 2;
        remaining -= (int)n;
        if (n == 0 || remaining < 0)
            return 0;
        if (typ == 0xF0) {
            for (unsigned i = 0; i < n; i++, pos += 3) {
                NEED(3);
                put_rec(table, 2, 0, rd16le(d + pos + 1), d[pos], 0);
            }
            total += n;
            continue;
        }
        if (typ & 0xE0)
            return 0;
        unsigned addr = ADDR_KIND_TO_NE[typ & 3], target = (typ >> 3) & 3;
        unsigned rtype = target | (typ & 4 ? 4 : 0), hdr;
        if (target == 0) {
            NEED(1);
            hdr = d[pos++];
        } else {
            NEED(2);
            hdr = rd16le(d + pos);
            pos += 2;
            if (target == 3)
                hdr &= 0xFF;
        }
        for (unsigned i = 0; i < n; i++) {
            if (target == 3) {
                NEED(2);
                put_rec(table, addr, rtype, rd16le(d + pos), hdr, 0);
                pos += 2;
            } else {
                NEED(4);
                put_rec(table, addr, rtype, rd16le(d + pos), hdr, rd16le(d + pos + 2));
                pos += 4;
            }
        }
        total += n;
    }
#undef NEED
    wr16le(table->p, (uint16_t)total);
    return pos;
}

/* ---- the whole file (unpack.py unpack) ------------------------------------------------------- */

int optloader_unpack(const uint8_t *d, size_t size, uint8_t **out_p, size_t *out_n, char *err, size_t errcap)
{
#define FAIL(...) do { snprintf(err, errcap, __VA_ARGS__); goto fail; } while (0)
    Buf out = { 0 }, seg_img[256] = { { 0 } }, seg_rel[256] = { { 0 } };
    if (size < 0x40 || d[0] != 'M' || d[1] != 'Z')
        FAIL("not an MZ executable");
    size_t ne = (size_t)(d[0x3C] | d[0x3D] << 8 | d[0x3E] << 16 | (size_t)d[0x3F] << 24);
    if (ne + 0x40 > size || d[ne] != 'N' || d[ne + 1] != 'E')
        FAIL("not an NE executable");
    unsigned flags = rd16le(d + ne + 0x0C), nseg = rd16le(d + ne + 0x1C);
    size_t seg_table = ne + rd16le(d + ne + 0x22);
    unsigned align = rd16le(d + ne + 0x32);
    if (!align)
        align = 9;
    if (!(flags & NE_FLAG_SELFLOAD))
        FAIL("not self-loading");
    if (nseg == 0 || nseg > 255 || seg_table + 8 * nseg > size)
        FAIL("bad segment table");
#define SEG(n, k) rd16le(d + seg_table + 8 * ((n) - 1) + 2 * (k))

    /* segment 1, the loader: its tail at 0x1E7 is decompressed in place (BootApp) */
    {
        size_t start = (size_t)SEG(1, 0) << align, length = SEG(1, 1);
        if (start + length > size || length < 0x1E7 || d[start] != 'A' || d[start + 1] != '0')
            FAIL("segment 1 is not an OPTLOADER self-loader");
        if (0x1E7 + rd16le(d + start + 0x2E) != length)
            FAIL("unexpected loader layout");
        buf_put(&seg_img[1], d + start, 0x1E7);
        size_t end = decompress(d, start + 0x1E7, start + length, &seg_img[1]);
        if (end != start + length)
            FAIL("loader tail did not decompress cleanly");
        if (SEG(1, 2) & SEG_RELOCINFO) {
            unsigned nrel = rd16le(d + start + length);
            buf_put(&seg_rel[1], d + start + length, 2 + 8 * (size_t)nrel);
        }
    }
    for (unsigned n = 2; n <= nseg; n++) {
        size_t sector = SEG(n, 0), length = SEG(n, 1);
        size_t minalloc = SEG(n, 3) ? SEG(n, 3) : 0x10000;
        if (!sector)
            continue;
        size_t start = sector << align, disk_len = length ? length : 0x10000;
        if (start + 2 > size)
            FAIL("segment %u outside the file", n);
        unsigned count = rd16le(d + start);
        size_t pos = decompress(d, start + 2, size, &seg_img[n]);
        if (!pos)
            FAIL("segment %u did not decompress", n);
        if (seg_img[n].n > minalloc)
            FAIL("segment %u larger than its allocation", n);
        if (count) {
            pos = parse_fixups(d, pos, size, count, &seg_rel[n]);
            if (!pos)
                FAIL("segment %u: bad fixup records", n);
        }
        if (pos > start + disk_len)
            FAIL("segment %u: packed data overruns the segment", n);
    }

    /* the original bytes, the segments appended with standard relocation tables, the segment
     * table repointed, self-loading cleared, and the Director trailer re-appended */
    if (!buf_put(&out, d, size))
        FAIL("out of memory");
    wr16le(out.p + ne + 0x0C, (uint16_t)(flags & ~NE_FLAG_SELFLOAD));
    size_t unit = (size_t)1 << align;
    for (unsigned n = 1; n <= nseg; n++) {
        if (!seg_img[n].n)
            continue;
        static const uint8_t zeros[512];
        size_t pad = (unit - out.n % unit) % unit;
        while (pad) {
            size_t k = pad < sizeof(zeros) ? pad : sizeof(zeros);
            buf_put(&out, zeros, k);
            pad -= k;
        }
        size_t new_sector = out.n >> align;
        if (new_sector > 0xFFFF)
            FAIL("output too large");
        buf_put(&out, seg_img[n].p, seg_img[n].n);
        if (seg_rel[n].n)
            buf_put(&out, seg_rel[n].p, seg_rel[n].n);
        unsigned sflags = SEG(n, 2) & ~SEG_PACKED;
        sflags = seg_rel[n].n ? sflags | SEG_RELOCINFO : sflags & ~SEG_RELOCINFO;
        uint8_t *e = out.p + seg_table + 8 * (n - 1);
        wr16le(e, (uint16_t)new_sector);
        wr16le(e + 2, (uint16_t)(seg_img[n].n & 0xFFFF));
        wr16le(e + 4, (uint16_t)sflags);
    }
    if (!buf_put(&out, d + size - TRAILER_LEN, TRAILER_LEN))
        FAIL("out of memory");
    for (unsigned n = 0; n < 256; n++) {
        free(seg_img[n].p);
        free(seg_rel[n].p);
    }
    *out_p = out.p;
    *out_n = out.n;
    return 1;
fail:
    for (unsigned n = 0; n < 256; n++) {
        free(seg_img[n].p);
        free(seg_rel[n].p);
    }
    free(out.p);
    return 0;
#undef SEG
#undef FAIL
}
