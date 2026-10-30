/* GDI drawing: raster operations, BitBlt/StretchBlt/PatBlt, DIBs, bitmaps, lines and
 * filled shapes.
 *
 * Every surface holds one byte per pixel, so ternary raster operations are evaluated
 * bitwise on pixel values, as a palettized display driver does. Monochrome <-> colour
 * conversion follows Windows: a monochrome source draws white pixels in the destination's
 * background colour and black in its text colour; a colour source becomes white where it
 * matches the source DC's background colour.
 */
#include <stdio.h>
#include <string.h>

#include "gdi.h"

/* ---- raster operations ------------------------------------------------------------------- */

static inline uint8_t rop3(uint8_t r, uint8_t P, uint8_t S, uint8_t D)
{
    switch (r) {
    case 0x00: return 0x00;
    case 0xFF: return 0xFF;
    case 0xCC: return S;
    case 0xF0: return P;
    case 0xAA: return D;
    case 0x55: return (uint8_t)~D;
    case 0x33: return (uint8_t)~S;
    case 0x0F: return (uint8_t)~P;
    case 0x88: return S & D;
    case 0xEE: return S | D;
    case 0x66: return S ^ D;
    case 0x44: return S & (uint8_t)~D;
    case 0x22: return (uint8_t)~S & D;
    case 0xBB: return (uint8_t)~S | D;
    case 0x11: return (uint8_t)~(S | D);
    case 0xC0: return P & S;
    case 0x5A: return P ^ D;
    case 0xA0: return P & D;
    case 0xFA: return P | D;
    case 0xB8: return (S & D) | ((uint8_t)~S & P);
    case 0xE2: return (S & P) | ((uint8_t)~S & D);
    }
    uint8_t res = 0;
    for (int k = 0; k < 8; k++)
        if (r >> k & 1)
            res |= (uint8_t)((k & 4 ? P : ~P) & (k & 2 ? S : ~S) & (k & 1 ? D : ~D));
    return res;
}

static int rop_uses_src(uint8_t r) { return ((r >> 2) & 0x33) != (r & 0x33); }
static int rop_uses_pat(uint8_t r) { return ((r >> 4) & 0x0F) != (r & 0x0F); }

/* binary raster operation (R2_xxx, 1..16) as a ternary one on P and D */
static uint8_t rop2_to_rop3(int r2)
{
    int v = (r2 - 1) & 15;
    uint8_t r = 0;
    for (int k = 0; k < 8; k++) {
        int p = (k >> 2) & 1, d = k & 1;
        if (v >> (p << 1 | d) & 1)
            r |= (uint8_t)(1 << k);
    }
    return r;
}

/* ---- patterns ---------------------------------------------------------------------------- */

enum { PAT_NONE, PAT_SOLID, PAT_BITS };

typedef struct Pat {
    int kind;
    uint8_t solid;
    uint8_t bits[64];            /* 8x8 pattern, already in destination pixel values */
    int ox, oy;                  /* brush origin, surface coordinates */
} Pat;

static const uint8_t g_hatch[6][8] = {
    { 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00 },   /* HS_HORIZONTAL */
    { 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08 },   /* HS_VERTICAL */
    { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 },   /* HS_FDIAGONAL */
    { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 },   /* HS_BDIAGONAL */
    { 0x08, 0x08, 0x08, 0xFF, 0x08, 0x08, 0x08, 0x08 },   /* HS_CROSS */
    { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },   /* HS_DIAGCROSS */
};

static int pat_setup(Pat *p, DC *dc, uint16_t hbrush, const Target *t)
{
    Brush *b = h_get(hbrush, HT_BRUSH);
    p->kind = PAT_NONE;
    p->ox = t->ox - dc->vp_x + dc->win_x + dc->brush_x;
    p->oy = t->oy - dc->vp_y + dc->win_y + dc->brush_y;
    if (!b || b->style == BS_NULL)
        return 0;
    if (b->style == BS_SOLID) {
        p->kind = PAT_SOLID;
        p->solid = dc_color(dc, b->color);
        return 1;
    }
    p->kind = PAT_BITS;
    if (b->style == BS_HATCHED) {
        uint8_t fg = dc_color(dc, b->color), bg = dc_color(dc, dc->bk_color);
        const uint8_t *h = g_hatch[(unsigned)b->hatch < 6 ? b->hatch : 0];
        for (int i = 0; i < 64; i++)
            p->bits[i] = (h[i >> 3] >> (7 - (i & 7)) & 1) ? fg : bg;
        return 1;
    }
    int dst_mono = t->s->mono;
    uint8_t fg = dc_color(dc, dc->text_color), bg = dc_color(dc, dc->bk_color);
    uint8_t key = dc_color(dc, dc->bk_color);
    for (int i = 0; i < 64; i++) {
        uint8_t v = b->pat.bits ? b->pat.bits[i] : 0;
        if (b->pat.mono && !dst_mono)
            v = v ? bg : fg;
        else if (!b->pat.mono && dst_mono)
            v = v == key ? 0xFF : 0x00;
        p->bits[i] = v;
    }
    return 1;
}

static inline uint8_t pat_at(const Pat *p, int x, int y)
{
    if (p->kind == PAT_SOLID)
        return p->solid;
    if (p->kind == PAT_BITS)
        return p->bits[((y - p->oy) & 7) * 8 + ((x - p->ox) & 7)];
    return 0;
}

/* ---- the blitter ------------------------------------------------------------------------- */

typedef struct Src {
    const Surface *s;
    IRect r;                     /* source rectangle (may differ in size: stretch; negative: mirror) */
    int conv;                    /* 0 none, 1 mono -> colour, 2 colour -> mono, 3 colour -> colour table */
    uint8_t fg, bg;              /* mono -> colour: black -> fg, white -> bg */
    uint8_t key;                 /* colour -> mono: key -> white */
    uint8_t xlat[256];           /* colour -> colour between surfaces with different colour tables */
} Src;

static inline uint8_t src_conv(const Src *s, uint8_t v)
{
    if (s->conv == 1)
        return v ? s->bg : s->fg;
    if (s->conv == 2)
        return v == s->key ? 0xFF : 0x00;
    if (s->conv == 3)
        return s->xlat[v];
    return v;
}

/* Colour-to-colour translation from one surface's pixel values to another's. */
static void src_xlat(Src *s, const Surface *src, const Surface *dst)
{
    if (src->mono || dst->mono || (!src->map && !dst->inv))
        return;
    for (int v = 0; v < 256; v++) {
        uint8_t sys = src->map ? src->map[v] : (uint8_t)v;
        s->xlat[v] = dst->inv ? dst->inv[sys] : sys;
    }
    s->conv = 3;
}

/* Blit into t (dr in surface coordinates). src may be NULL; pat may be NULL. */
static void blit(const Target *t, IRect dr, const Src *src_in, uint8_t rop, const Pat *pat)
{
    Surface *d = t->s;
    int dw = dr.r - dr.l, dh = dr.b - dr.t;
    Src srcv, *src = NULL;
    Surface tmp = { 0 };
    int mirror_x = 0, mirror_y = 0;
    if (dw < 0) { int x = dr.l; dr.l = dr.r; dr.r = x; dw = -dw; mirror_x ^= 1; }
    if (dh < 0) { int y = dr.t; dr.t = dr.b; dr.b = y; dh = -dh; mirror_y ^= 1; }
    if (!dw || !dh)
        return;
    int sw = 0, sh = 0;
    if (src_in && rop_uses_src(rop)) {
        srcv = *src_in;
        src = &srcv;
        sw = src->r.r - src->r.l;
        sh = src->r.b - src->r.t;
        if (sw < 0) { int x = src->r.l; src->r.l = src->r.r; src->r.r = x; sw = -sw; mirror_x ^= 1; }
        if (sh < 0) { int y = src->r.t; src->r.t = src->r.b; src->r.b = y; sh = -sh; mirror_y ^= 1; }
        if (!sw || !sh)
            return;
        if (src->s == d) {                         /* overlapping copy within a surface */
            surface_init(&tmp, sw, sh, d->mono);
            for (int y = 0; y < sh; y++) {
                int sy = src->r.t + y;
                for (int x = 0; x < sw; x++) {
                    int sx = src->r.l + x;
                    tmp.bits[y * sw + x] = (sx >= 0 && sy >= 0 && sx < d->w && sy < d->h) ? surf_row(d, sy)[sx] : 0;
                }
            }
            src->s = &tmp;
            src->r = irect(0, 0, sw, sh);
        }
    }
    int stretch = src && (sw != dw || sh != dh);
    IRect bounds = irect(0, 0, d->w, d->h);
    for (int i = 0; i < t->clip->n; i++) {
        IRect x;
        if (!rect_intersect(&x, &t->clip->r[i], &dr) || !rect_intersect(&x, &x, &bounds))
            continue;
        for (int y = x.t; y < x.b; y++) {
            uint8_t *row = surf_row(d, y);
            const uint8_t *srow = NULL;
            int sy = 0;
            if (src) {
                int ry = mirror_y ? (dh - 1 - (y - dr.t)) : (y - dr.t);
                sy = src->r.t + (stretch ? (int)((int64_t)ry * sh / dh) : ry);
                if (sy < 0 || sy >= src->s->h)
                    continue;
                srow = surf_row(src->s, sy);
            }
            /* fast paths */
            if (src && rop == 0xCC && !stretch && !mirror_x && !src->conv) {
                int sx0 = src->r.l + (x.l - dr.l), sx1 = sx0 + (x.r - x.l), xl = x.l;
                if (sx0 < 0) { xl -= sx0; sx0 = 0; }
                if (sx1 > src->s->w) sx1 = src->s->w;
                if (sx1 > sx0)
                    memcpy(row + xl, srow + sx0, (size_t)(sx1 - sx0));
                continue;
            }
            if (!src && pat && pat->kind == PAT_SOLID && rop == 0xF0) {
                memset(row + x.l, pat->solid, (size_t)(x.r - x.l));
                continue;
            }
            for (int xx = x.l; xx < x.r; xx++) {
                uint8_t S = 0, P = 0;
                if (src) {
                    int rx = mirror_x ? (dw - 1 - (xx - dr.l)) : (xx - dr.l);
                    int sx = src->r.l + (stretch ? (int)((int64_t)rx * sw / dw) : rx);
                    if (sx < 0 || sx >= src->s->w)
                        continue;
                    S = src_conv(src, srow[sx]);
                }
                if (pat)
                    P = pat_at(pat, xx, y);
                row[xx] = rop3(rop, P, S, row[xx]);
            }
        }
    }
    free(tmp.bits);
    if (d == &g_screen)
        video_dirty();
}

void blit_fill(const Target *t, IRect r, uint8_t pixel)
{
    Pat p = { PAT_SOLID, pixel, { 0 }, 0, 0 };
    blit(t, r, NULL, 0xF0, &p);
}

/* Draw the set bits of a 1-byte-per-pixel mask in fg (text glyphs). */
void blit_mono_mask(const Target *t, int x0, int y0, const uint8_t *mask, int mw, int mh, int stride, uint8_t fg)
{
    Surface *d = t->s;
    IRect dr = irect(x0, y0, x0 + mw, y0 + mh), bounds = irect(0, 0, d->w, d->h);
    for (int i = 0; i < t->clip->n; i++) {
        IRect x;
        if (!rect_intersect(&x, &t->clip->r[i], &dr) || !rect_intersect(&x, &x, &bounds))
            continue;
        for (int y = x.t; y < x.b; y++) {
            const uint8_t *m = mask + (size_t)(y - y0) * stride;
            uint8_t *row = surf_row(d, y);
            for (int xx = x.l; xx < x.r; xx++)
                if (m[xx - x0])
                    row[xx] = fg;
        }
    }
    if (d == &g_screen)
        video_dirty();
}

/* ---- helpers on DCs --------------------------------------------------------------------- */

static IRect to_surface(const Target *t, IRect r)
{
    rect_offset(&r, t->ox, t->oy);
    return r;
}

void gdi_fill_rect(DC *dc, IRect r, uint16_t hbrush)
{
    Target t;
    Pat p;
    if (!dc || !dc_target(dc, &t) || !pat_setup(&p, dc, hbrush, &t))
        return;
    blit(&t, to_surface(&t, r), NULL, 0xF0, &p);
}

void gdi_invert_rect(DC *dc, IRect r)
{
    Target t;
    if (!dc || !dc_target(dc, &t))
        return;
    blit(&t, to_surface(&t, r), NULL, 0x55, NULL);
}

void gdi_frame_rect(DC *dc, IRect r, uint16_t hbrush)
{
    if (rect_empty(&r))
        return;
    gdi_fill_rect(dc, irect(r.l, r.t, r.r, r.t + 1), hbrush);
    gdi_fill_rect(dc, irect(r.l, r.b - 1, r.r, r.b), hbrush);
    gdi_fill_rect(dc, irect(r.l, r.t + 1, r.l + 1, r.b - 1), hbrush);
    gdi_fill_rect(dc, irect(r.r - 1, r.t + 1, r.r, r.b - 1), hbrush);
}

/* Source description for a blit from DC `sdc` into a target. */
static int src_setup(Src *s, DC *sdc, DC *ddc, const Target *dt, IRect sr_logical, Target *st)
{
    if (!sdc || !dc_target(sdc, st)) {
        /* a source DC with nothing visible still has its surface */
        if (!sdc || sdc->kind == DC_INFO)
            return 0;
    }
    s->s = st->s;
    s->r = to_surface(st, sr_logical);
    s->conv = 0;
    src_xlat(s, st->s, dt->s);
    if (st->s->mono && !dt->s->mono) {
        s->conv = 1;
        s->fg = dc_color(ddc, ddc->text_color);
        s->bg = dc_color(ddc, ddc->bk_color);
    } else if (!st->s->mono && dt->s->mono) {
        s->conv = 2;
        s->key = dc_color(sdc, sdc->bk_color);
    }
    return 1;
}

/* ---- API: blits -------------------------------------------------------------------------- */

static void PatBlt(CPU *c)             /* BOOL PatBlt(HDC, int x, int y, int w, int h, DWORD rop) */
{
    uint32_t a[6];
    uint16_t n = api_args(c, "wwwwwl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    Pat p;
    if (dc && dc_target(dc, &t)) {
        uint8_t rop = (uint8_t)(a[5] >> 16);
        pat_setup(&p, dc, dc->hbrush, &t);
        int x = (int16_t)a[1], y = (int16_t)a[2];
        blit(&t, to_surface(&t, irect(x, y, x + (int16_t)a[3], y + (int16_t)a[4])), NULL, rop, &p);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static const char *dc_desc(DC *dc)
{
    static char buf[2][48];
    static int n;
    char *b = buf[n++ & 1];
    if (!dc)
        return "null";
    static const char *const kinds[] = { "screen", "memory", "info", "dib" };
    if (dc->kind == DC_SCREEN)
        snprintf(b, 48, "screen(%04x)", dc->hwnd);
    else
        snprintf(b, 48, "%s", kinds[dc->kind]);
    return b;
}

static void do_bitblt(DC *dc, IRect dr, DC *sdc, IRect sr, uint32_t rop32)
{
    Target t, st;
    LOGD("blit %s (%d,%d %dx%d) <- %s (%d,%d %dx%d) rop %06x", dc_desc(dc), dr.l, dr.t, dr.r - dr.l, dr.b - dr.t,
         dc_desc(sdc), sr.l, sr.t, sr.r - sr.l, sr.b - sr.t, rop32 >> 8);
    if (!dc || !dc_target(dc, &t))
        return;
    uint8_t rop = (uint8_t)(rop32 >> 16);
    Src s, *sp = NULL;
    if (rop_uses_src(rop)) {
        memset(&st, 0, sizeof(st));
        if (!src_setup(&s, sdc, dc, &t, sr, &st))
            return;
        sp = &s;
    }
    Pat p, *pp = NULL;
    if (rop_uses_pat(rop) && pat_setup(&p, dc, dc->hbrush, &t))
        pp = &p;
    else if (rop_uses_pat(rop)) {
        p.kind = PAT_SOLID;
        p.solid = 0;
        pp = &p;
    }
    blit(&t, to_surface(&t, dr), sp, rop, pp);
}

/* BOOL BitBlt(HDC, int x, int y, int w, int h, HDC src, int xs, int ys, DWORD rop) */
static void BitBlt(CPU *c)
{
    uint32_t a[9];
    uint16_t n = api_args(c, "wwwwwwwwl", a);
    int x = (int16_t)a[1], y = (int16_t)a[2], w = (int16_t)a[3], h = (int16_t)a[4];
    int xs = (int16_t)a[6], ys = (int16_t)a[7];
    do_bitblt(dc_get((uint16_t)a[0]), irect(x, y, x + w, y + h), dc_get((uint16_t)a[5]),
              irect(xs, ys, xs + w, ys + h), a[8]);
    c->a.x = 1;
    api_ret(c, n);
}

/* BOOL StretchBlt(HDC, int x, int y, int w, int h, HDC src, int xs, int ys, int ws, int hs, DWORD rop) */
static void StretchBlt(CPU *c)
{
    uint32_t a[11];
    uint16_t n = api_args(c, "wwwwwwwwwwl", a);
    int x = (int16_t)a[1], y = (int16_t)a[2], w = (int16_t)a[3], h = (int16_t)a[4];
    int xs = (int16_t)a[6], ys = (int16_t)a[7], ws = (int16_t)a[8], hs = (int16_t)a[9];
    do_bitblt(dc_get((uint16_t)a[0]), irect(x, y, x + w, y + h), dc_get((uint16_t)a[5]),
              irect(xs, ys, xs + ws, ys + hs), a[10]);
    c->a.x = 1;
    api_ret(c, n);
}

static void SetPixel(CPU *c)           /* COLORREF SetPixel(HDC, int, int, COLORREF) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    uint32_t r = 0xFFFFFFFF;
    if (dc && dc_target(dc, &t)) {
        uint8_t v = dc_color(dc, a[3]);
        int x = (int16_t)a[1] + t.ox, y = (int16_t)a[2] + t.oy;
        blit_fill(&t, irect(x, y, x + 1, y + 1), v);
        r = t.s->mono ? (v ? 0xFFFFFF : 0) : (uint32_t)g_syspal[v][0] | g_syspal[v][1] << 8 | g_syspal[v][2] << 16;
    }
    api_ret32(c, n, r);
}

static void GetPixel(CPU *c)           /* COLORREF GetPixel(HDC, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    uint32_t r = 0xFFFFFFFF;
    if (dc && dc_target(dc, &t)) {
        int x = (int16_t)a[1] + t.ox, y = (int16_t)a[2] + t.oy;
        if (rgn_pt_in(t.clip, x, y) && x >= 0 && y >= 0 && x < t.s->w && y < t.s->h) {
            uint8_t v = surf_row(t.s, y)[x];
            if (!t.s->mono && t.s->map)
                v = t.s->map[v];
            r = t.s->mono ? (v ? 0xFFFFFF : 0) : (uint32_t)g_syspal[v][0] | g_syspal[v][1] << 8 | g_syspal[v][2] << 16;
        }
    }
    api_ret32(c, n, r);
}

/* ---- lines and shapes ------------------------------------------------------------------- */

static void pen_point(const Target *t, int x, int y, int w, uint8_t rop, uint8_t color)
{
    Pat p = { PAT_SOLID, color, { 0 }, 0, 0 };
    if (w <= 1)
        blit(t, irect(x, y, x + 1, y + 1), NULL, rop, &p);
    else
        blit(t, irect(x - w / 2, y - w / 2, x - w / 2 + w, y - w / 2 + w), NULL, rop, &p);
}

/* line in surface coordinates, last point excluded */
static void draw_line(const Target *t, int x0, int y0, int x1, int y1, int w, uint8_t rop, uint8_t color)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    if (w <= 1 && (dy == 0 || dx == 0)) {        /* horizontal / vertical: one rectangle */
        Pat p = { PAT_SOLID, color, { 0 }, 0, 0 };
        if (dy == 0 && dx)
            blit(t, irect(x0 < x1 ? x0 : x1 + 1, y0, x0 < x1 ? x1 : x0 + 1, y0 + 1), NULL, rop, &p);
        else if (dx == 0 && dy)
            blit(t, irect(x0, y0 < y1 ? y0 : y1 + 1, x0 + 1, y0 < y1 ? y1 : y0 + 1), NULL, rop, &p);
        return;
    }
    while (x0 != x1 || y0 != y1) {
        pen_point(t, x0, y0, w, rop, color);
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

typedef struct PenCtx {
    Target t;
    int ok, w;
    uint8_t rop, color;
} PenCtx;

static int pen_setup(PenCtx *pc, DC *dc)
{
    pc->ok = 0;
    if (!dc || !dc_target(dc, &pc->t))
        return 0;
    Pen *p = h_get(dc->hpen, HT_PEN);
    if (!p || p->style == 5)                       /* PS_NULL */
        return 0;
    pc->w = p->width;
    pc->rop = rop2_to_rop3(dc->rop2);
    pc->color = dc_color(dc, p->color);
    pc->ok = 1;
    return 1;
}

static void LineTo(CPU *c)             /* BOOL LineTo(HDC, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    PenCtx pc;
    if (dc) {
        int x = (int16_t)a[1], y = (int16_t)a[2];
        if (pen_setup(&pc, dc))
            draw_line(&pc.t, dc->cur_x + pc.t.ox, dc->cur_y + pc.t.oy, x + pc.t.ox, y + pc.t.oy, pc.w, pc.rop, pc.color);
        dc->cur_x = x;
        dc->cur_y = y;
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void polyline(DC *dc, const int *xy, int count, int close)
{
    PenCtx pc;
    if (count < 2 || !pen_setup(&pc, dc))
        return;
    for (int i = 0; i + 1 < count + (close ? 1 : 0); i++) {
        int j = (i + 1) % count;
        draw_line(&pc.t, xy[2 * i] + pc.t.ox, xy[2 * i + 1] + pc.t.oy, xy[2 * j] + pc.t.ox, xy[2 * j + 1] + pc.t.oy,
                  pc.w, pc.rop, pc.color);
    }
}

/* Fill a region (device coordinates) with the DC's brush under its ROP2. */
static void fill_region_brush(DC *dc, const Region *g, uint16_t hbrush, uint8_t rop)
{
    Target t;
    Pat p;
    if (!dc_target(dc, &t) || !pat_setup(&p, dc, hbrush, &t))
        return;
    for (int i = 0; i < g->n; i++)
        blit(&t, to_surface(&t, g->r[i]), NULL, rop, &p);
}

static int *points_from_guest(uint32_t far, int count)
{
    int *xy = malloc(sizeof(int) * 2 * (size_t)(count > 0 ? count : 1));
    uint16_t s = FAR_SEL(far), o = FAR_OFF(far);
    for (int i = 0; i < count; i++) {
        xy[2 * i] = (int16_t)rd16(s, (uint16_t)(o + 4 * i));
        xy[2 * i + 1] = (int16_t)rd16(s, (uint16_t)(o + 4 * i + 2));
    }
    return xy;
}

static void Polyline(CPU *c)           /* BOOL Polyline(HDC, const POINT FAR *, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int count = (int16_t)a[2];
    if (dc && count > 0) {
        int *xy = points_from_guest(a[1], count);
        polyline(dc, xy, count, 0);
        free(xy);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void poly_polygon(DC *dc, int *xy, int *counts, int np)
{
    Region g;
    rgn_init(&g);
    rgn_polygon(&g, xy, counts, np, dc->poly_fill_mode);
    fill_region_brush(dc, &g, dc->hbrush, rop2_to_rop3(dc->rop2));
    rgn_free(&g);
    int *p = xy;
    for (int i = 0; i < np; i++) {
        polyline(dc, p, counts[i], 1);
        p += 2 * counts[i];
    }
}

static void Polygon(CPU *c)            /* BOOL Polygon(HDC, const POINT FAR *, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int count = (int16_t)a[2];
    if (dc && count > 1) {
        int *xy = points_from_guest(a[1], count);
        poly_polygon(dc, xy, &count, 1);
        free(xy);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void PolyPolygon(CPU *c)        /* BOOL PolyPolygon(HDC, const POINT FAR *, const int FAR *, int) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wllw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int np = (int16_t)a[3], total = 0;
    if (dc && np > 0) {
        int *counts = malloc(sizeof(int) * (size_t)np);
        for (int i = 0; i < np; i++) {
            counts[i] = (int16_t)rd16(FAR_SEL(a[2]), (uint16_t)(FAR_OFF(a[2]) + 2 * i));
            total += counts[i];
        }
        int *xy = points_from_guest(a[1], total);
        poly_polygon(dc, xy, counts, np);
        free(xy);
        free(counts);
    }
    c->a.x = 1;
    api_ret(c, n);
}

/* Outline of a region (its boundary pixels) in the pen colour. */
static void outline_region(DC *dc, const Region *g)
{
    PenCtx pc;
    if (!pen_setup(&pc, dc))
        return;
    for (int i = 0; i < g->n; i++) {
        IRect r = g->r[i];
        for (int y = r.t; y < r.b; y++)
            for (int x = r.l; x < r.r; x++)
                if (!rgn_pt_in(g, x - 1, y) || !rgn_pt_in(g, x + 1, y) || !rgn_pt_in(g, x, y - 1) || !rgn_pt_in(g, x, y + 1))
                    pen_point(&pc.t, x + pc.t.ox, y + pc.t.oy, pc.w, pc.rop, pc.color);
    }
}

static void Rectangle(CPU *c)          /* BOOL Rectangle(HDC, int l, int t, int r, int b) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc) {
        IRect r = irect((int16_t)a[1], (int16_t)a[2], (int16_t)a[3], (int16_t)a[4]);
        Pen *p = h_get(dc->hpen, HT_PEN);
        int nopen = !p || p->style == 5;
        Region g;
        rgn_init(&g);
        rgn_set_rect(&g, nopen ? irect(r.l, r.t, r.r - 1, r.b - 1) : irect(r.l + 1, r.t + 1, r.r - 1, r.b - 1));
        fill_region_brush(dc, &g, dc->hbrush, rop2_to_rop3(dc->rop2));
        rgn_free(&g);
        if (!nopen) {
            int xy[8] = { r.l, r.t, r.r - 1, r.t, r.r - 1, r.b - 1, r.l, r.b - 1 };
            polyline(dc, xy, 4, 1);
        }
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void shape_region(CPU *c, int kind)     /* Ellipse / RoundRect */
{
    uint32_t a[7];
    uint16_t n = api_args(c, kind ? "wwwwwww" : "wwwww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc) {
        IRect r = irect((int16_t)a[1], (int16_t)a[2], (int16_t)a[3], (int16_t)a[4]);
        Region g;
        rgn_init(&g);
        if (kind)
            rgn_roundrect(&g, r, (int16_t)a[5], (int16_t)a[6]);
        else
            rgn_ellipse(&g, r);
        fill_region_brush(dc, &g, dc->hbrush, rop2_to_rop3(dc->rop2));
        outline_region(dc, &g);
        rgn_free(&g);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void Ellipse(CPU *c) { shape_region(c, 0); }
static void RoundRect(CPU *c) { shape_region(c, 1); }

static Region *dc_region(DC *dc, uint16_t hrgn, Region *tmp)
{
    Region *g = h_get(hrgn, HT_RGN);
    if (!g)
        return NULL;
    rgn_init(tmp);
    rgn_copy(tmp, g);
    return tmp;
}

static void FillRgn(CPU *c)            /* BOOL FillRgn(HDC, HRGN, HBRUSH) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Region tmp;
    if (dc && dc_region(dc, (uint16_t)a[1], &tmp)) {
        fill_region_brush(dc, &tmp, (uint16_t)a[2], 0xF0);
        rgn_free(&tmp);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void PaintRgn(CPU *c)           /* BOOL PaintRgn(HDC, HRGN) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Region tmp;
    if (dc && dc_region(dc, (uint16_t)a[1], &tmp)) {
        fill_region_brush(dc, &tmp, dc->hbrush, rop2_to_rop3(dc->rop2));
        rgn_free(&tmp);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void InvertRgn(CPU *c)          /* BOOL InvertRgn(HDC, HRGN) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Region tmp;
    Target t;
    if (dc && dc_region(dc, (uint16_t)a[1], &tmp)) {
        if (dc_target(dc, &t))
            for (int i = 0; i < tmp.n; i++)
                blit(&t, to_surface(&t, tmp.r[i]), NULL, 0x55, NULL);
        rgn_free(&tmp);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void FrameRgn(CPU *c)           /* BOOL FrameRgn(HDC, HRGN, HBRUSH, int w, int h) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Region tmp;
    if (dc && dc_region(dc, (uint16_t)a[1], &tmp)) {
        /* frame = region minus the region shrunk by w x h */
        int fw = (int16_t)a[3], fh = (int16_t)a[4];
        Region inner, sh;
        rgn_init(&inner);
        rgn_init(&sh);
        rgn_copy(&inner, &tmp);
        for (int dx = -fw; dx <= fw; dx += fw ? fw : 1)
            for (int dy = -fh; dy <= fh; dy += fh ? fh : 1) {
                rgn_copy(&sh, &tmp);
                rgn_offset(&sh, dx, dy);
                rgn_combine(&inner, &inner, &sh, RGN_AND);
                if (!fh) break;
            }
        rgn_combine(&tmp, &tmp, &inner, RGN_DIFF);
        fill_region_brush(dc, &tmp, (uint16_t)a[2], 0xF0);
        rgn_free(&inner);
        rgn_free(&sh);
        rgn_free(&tmp);
    }
    c->a.x = 1;
    api_ret(c, n);
}

/* ---- USER rectangle drawing ------------------------------------------------------------- */

static IRect dc_rect(DC *dc, uint32_t far)
{
    (void)dc;
    return rd_rect(far);                          /* logical, like every drawing call */
}

static void FillRect(CPU *c)           /* int FillRect(HDC, const RECT FAR *, HBRUSH) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc) {
        uint16_t br = (uint16_t)a[2];
        if (br && br <= 21)
            br = gdi_solid_brush(user_sys_color(br - 1));
        gdi_fill_rect(dc, dc_rect(dc, a[1]), br);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void FrameRect(CPU *c)          /* int FrameRect(HDC, const RECT FAR *, HBRUSH) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc)
        gdi_frame_rect(dc, dc_rect(dc, a[1]), (uint16_t)a[2]);
    c->a.x = 1;
    api_ret(c, n);
}

static void InvertRect(CPU *c)         /* void InvertRect(HDC, const RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc)
        gdi_invert_rect(dc, dc_rect(dc, a[1]));
    api_ret(c, n);
}

static void DrawFocusRect(CPU *c)      /* void DrawFocusRect(HDC, const RECT FAR *): dotted XOR */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    if (dc && dc_target(dc, &t)) {
        IRect r = to_surface(&t, dc_rect(dc, a[1]));
        for (int x = r.l; x < r.r; x++)
            if (!((x + r.t) & 1)) {
                blit(&t, irect(x, r.t, x + 1, r.t + 1), NULL, 0x55, NULL);
                blit(&t, irect(x, r.b - 1, x + 1, r.b), NULL, 0x55, NULL);
            }
        for (int y = r.t + 1; y < r.b - 1; y++)
            if (!((y + r.l) & 1)) {
                blit(&t, irect(r.l, y, r.l + 1, y + 1), NULL, 0x55, NULL);
                blit(&t, irect(r.r - 1, y, r.r, y + 1), NULL, 0x55, NULL);
            }
    }
    api_ret(c, n);
}

/* BOOL ScrollDC(HDC, int dx, int dy, const RECT FAR *scroll, const RECT FAR *clip, HRGN update,
 *               RECT FAR *update rect) */
static void ScrollDC(CPU *c)
{
    uint32_t a[7];
    uint16_t n = api_args(c, "wwwllwl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    if (dc && dc_target(dc, &t)) {
        int dx = (int16_t)a[1], dy = (int16_t)a[2];
        IRect sr = a[3] ? dc_rect(dc, a[3]) : irect(-t.ox, -t.oy, t.s->w - t.ox, t.s->h - t.oy);
        IRect dr = sr;
        rect_offset(&dr, dx, dy);
        if (a[4]) {
            IRect cr = dc_rect(dc, a[4]);
            rect_intersect(&dr, &dr, &cr);
        }
        Src s = { t.s, to_surface(&t, irect(dr.l - dx, dr.t - dy, dr.r - dx, dr.b - dy)), 0, 0, 0, 0, { 0 } };
        blit(&t, to_surface(&t, dr), &s, 0xCC, NULL);
        /* the uncovered part */
        Region upd, moved;
        rgn_init(&upd);
        rgn_init(&moved);
        rgn_set_rect(&upd, sr);
        rgn_set_rect(&moved, dr);
        rgn_combine(&upd, &upd, &moved, RGN_DIFF);
        Region *hu = h_get((uint16_t)a[5], HT_RGN);
        if (hu)
            rgn_copy(hu, &upd);
        if (a[6])
            wr_rect(a[6], upd.box);
        rgn_free(&upd);
        rgn_free(&moved);
    }
    c->a.x = 1;
    api_ret(c, n);
}

/* ---- DIBs -------------------------------------------------------------------------------- */

int dib_info(const uint8_t *info, int *w, int *h, int *bpp, int *ncolors, int *hdr_size, int *entry_size)
{
    uint32_t size = rd32_host(info);
    int comp = 0;
    if (size == 12) {                               /* BITMAPCOREHEADER */
        *w = (int16_t)rd16_host(info + 4);
        *h = (int16_t)rd16_host(info + 6);
        *bpp = rd16_host(info + 10);
        *entry_size = 3;
        *ncolors = *bpp <= 8 ? 1 << *bpp : 0;
    } else {
        *w = (int32_t)rd32_host(info + 4);
        *h = (int32_t)rd32_host(info + 8);
        *bpp = rd16_host(info + 14);
        comp = (int)rd32_host(info + 16);
        uint32_t used = size >= 36 ? rd32_host(info + 32) : 0;
        *entry_size = 4;
        *ncolors = *bpp <= 8 ? (used ? (int)used : 1 << *bpp) : 0;
    }
    *hdr_size = (int)size;
    return comp;
}

static int decode_rows(const uint8_t *bits, int comp, int w, int h, int bpp, const uint8_t map[256], DC *dc,
                       int mono_dst, int start, int nlines, Surface *out)
{
    int ah = h < 0 ? -h : h;
    int top_down = h < 0;
    int stride = ((w * bpp + 31) / 32) * 4;
    if (nlines < 0 || start + nlines > ah)
        nlines = ah - start;
    if (comp == 0) {
        for (int i = 0; i < nlines; i++) {
            int dib_row = start + i;                    /* rows counted from the bottom */
            int y = top_down ? dib_row : ah - 1 - dib_row;
            if (y < 0 || y >= ah)
                continue;
            const uint8_t *src = bits + (size_t)i * stride;
            uint8_t *dst = out->bits + (size_t)y * out->stride;
            switch (bpp) {
            case 1:
                for (int x = 0; x < w; x++)
                    dst[x] = map[(src[x >> 3] >> (7 - (x & 7))) & 1];
                break;
            case 4:
                for (int x = 0; x < w; x++)
                    dst[x] = map[(src[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
                break;
            case 8:
                for (int x = 0; x < w; x++)
                    dst[x] = map[src[x]];
                break;
            case 24: case 32: {
                int bytes = bpp / 8;
                uint32_t last = 0xFFFFFFFF;
                uint8_t lastv = 0;
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = src + x * bytes;
                    uint32_t rgb = (uint32_t)p[2] | (uint32_t)p[1] << 8 | (uint32_t)p[0] << 16;
                    if (rgb != last) {
                        last = rgb;
                        lastv = mono_dst ? ((p[2] * 30 + p[1] * 59 + p[0] * 11) >= 12800 ? 0xFF : 0)
                                         : dib_rgb_to_sys(dc, p[2], p[1], p[0]);
                    }
                    dst[x] = lastv;
                }
                break;
            }
            default:
                return 0;
            }
        }
        return 1;
    }
    if (comp == 1 || comp == 2) {                     /* RLE8 / RLE4, bottom-up */
        int x = 0, row = 0;
        const uint8_t *p = bits;
        for (;;) {
            uint8_t cnt = *p++, v = *p++;
            if (cnt) {
                for (int i = 0; i < cnt; i++, x++) {
                    int idx = comp == 1 ? v : (i & 1) ? (v & 15) : (v >> 4);
                    if (x < w && row < ah)
                        out->bits[(size_t)(ah - 1 - row) * out->stride + x] = map[idx];
                }
            } else if (v == 0) {
                x = 0;
                row++;
            } else if (v == 1) {
                break;
            } else if (v == 2) {
                x += *p++;
                row += *p++;
            } else {
                for (int i = 0; i < v; i++, x++) {
                    int idx = comp == 1 ? p[i] : (i & 1) ? (p[i >> 1] & 15) : (p[i >> 1] >> 4);
                    if (x < w && row < ah)
                        out->bits[(size_t)(ah - 1 - row) * out->stride + x] = map[idx];
                }
                int bytes = comp == 1 ? v : (v + 1) / 2;
                p += (bytes + 1) & ~1;
            }
            if (row >= ah)
                break;
        }
        return 1;
    }
    LOGW("DIB compression %d not supported", comp);
    return 0;
}

int dib_decode_host(DC *dc, const uint8_t *info, const uint8_t *bits, int usage, int mono_dst, Surface *out)
{
    int w, h, bpp, ncolors, hsize, esize;
    int comp = dib_info(info, &w, &h, &bpp, &ncolors, &hsize, &esize);
    uint8_t map[256];
    const uint8_t *table = info + hsize;
    dib_color_map(dc, table, ncolors, usage == 1 ? 2 : esize, usage == 1, mono_dst, map);
    if (!bits)
        bits = table + ncolors * (usage == 1 ? 2 : esize);
    if (!surface_init(out, w, h < 0 ? -h : h, mono_dst))
        return 0;
    if (!decode_rows(bits, comp, w, h, bpp, map, dc, mono_dst, 0, -1, out)) {
        free(out->bits);
        out->bits = NULL;
        return 0;
    }
    return 1;
}

int dib_decode_mono(DC *dc, uint32_t info_far, uint32_t bits_far, int usage, int start, int nlines, int mono_dst,
                    Surface *out)
{
    const uint8_t *info = (const uint8_t *)guest_ptr(info_far);
    int w, h, bpp, ncolors, hsize, esize;
    int comp = dib_info(info, &w, &h, &bpp, &ncolors, &hsize, &esize);
    uint8_t map[256];
    const uint8_t *table = info + hsize;
    int tsize = usage == 1 ? 2 : esize;
    dib_color_map(dc, table, ncolors, tsize, usage == 1, mono_dst, map);
    const uint8_t *bits = bits_far ? (const uint8_t *)guest_ptr(bits_far) : table + ncolors * tsize;
    if (!surface_init(out, w, h < 0 ? -h : h, mono_dst))
        return 0;
    if (!decode_rows(bits, comp, w, h, bpp, map, dc, mono_dst, start, nlines, out)) {
        free(out->bits);
        out->bits = NULL;
        return 0;
    }
    return 1;
}

int dib_decode(DC *dc, uint32_t info, uint32_t bits, int usage, int start, int nlines, Surface *out)
{
    return dib_decode_mono(dc, info, bits, usage, start, nlines, 0, out);
}

/* int StretchDIBits(HDC, int x, int y, int w, int h, int xs, int ys, int ws, int hs,
 *                   const void FAR *bits, const BITMAPINFO FAR *, UINT usage, DWORD rop) */
static void StretchDIBits(CPU *c)
{
    uint32_t a[13];
    uint16_t n = api_args(c, "wwwwwwwwwllwl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    int lines = 0;
    if (dc && dc_target(dc, &t)) {
        Surface s;
        if (dib_decode_mono(dc, a[10], a[9], (int)a[11], 0, -1, t.s->mono, &s)) {
            int x = (int16_t)a[1], y = (int16_t)a[2], w = (int16_t)a[3], h = (int16_t)a[4];
            int xs = (int16_t)a[5], ys = (int16_t)a[6], ws = (int16_t)a[7], hs = (int16_t)a[8];
            /* source y is measured from the bottom of the DIB */
            int top = s.h - ys - hs;
            Src src = { &s, irect(xs, top, xs + ws, top + hs), 0, 0, 0, 0, { 0 } };
            src_xlat(&src, &s, t.s);
            uint8_t rop = (uint8_t)(a[12] >> 16);
            Pat p, *pp = NULL;
            if (rop_uses_pat(rop) && pat_setup(&p, dc, dc->hbrush, &t))
                pp = &p;
            blit(&t, to_surface(&t, irect(x, y, x + w, y + h)), &src, rop, pp);
            lines = s.h;
            free(s.bits);
        }
    }
    c->a.x = (uint16_t)lines;
    api_ret(c, n);
}

/* int SetDIBitsToDevice(HDC, int x, int y, int w, int h, int xs, int ys, UINT start, UINT lines,
 *                       void FAR *bits, BITMAPINFO FAR *, UINT usage) */
static void SetDIBitsToDevice(CPU *c)
{
    uint32_t a[12];
    uint16_t n = api_args(c, "wwwwwwwwwllw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    if (dc && dc_target(dc, &t)) {
        Surface s;
        if (dib_decode_mono(dc, a[10], a[9], (int)a[11], (int)a[7], (int)a[8], t.s->mono, &s)) {
            int x = (int16_t)a[1], y = (int16_t)a[2], w = (int16_t)a[3], h = (int16_t)a[4];
            int xs = (int16_t)a[5], ys = (int16_t)a[6];
            int top = s.h - ys - h;
            Src src = { &s, irect(xs, top, xs + w, top + h), 0, 0, 0, 0, { 0 } };
            src_xlat(&src, &s, t.s);
            blit(&t, to_surface(&t, irect(x, y, x + w, y + h)), &src, 0xCC, NULL);
            free(s.bits);
        }
    }
    c->a.x = (uint16_t)a[8];
    api_ret(c, n);
}

/* int SetDIBits(HDC, HBITMAP, UINT start, UINT lines, const void FAR *, const BITMAPINFO FAR *, UINT) */
static void SetDIBits(CPU *c)
{
    uint32_t a[7];
    uint16_t n = api_args(c, "wwwwllw", a);
    Bitmap *bm = h_get((uint16_t)a[1], HT_BITMAP);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    Surface s;
    if (bm && dib_decode_mono(dc, a[5], a[4], (int)a[6], (int)a[2], (int)a[3], bm->s.mono, &s)) {
        int ah = s.h;
        int start = (int)a[2], lines = (int)a[3];
        if (start + lines > ah)
            lines = ah - start;
        for (int i = 0; i < lines; i++) {
            int y = ah - 1 - (start + i);
            if (y >= 0 && y < bm->s.h)
                memcpy(bm->s.bits + (size_t)y * bm->s.stride, s.bits + (size_t)y * s.stride,
                       (size_t)(s.w < bm->s.w ? s.w : bm->s.w));
        }
        free(s.bits);
        c->a.x = (uint16_t)lines;
        if (bm->dc)
            video_dirty();
    }
    api_ret(c, n);
}

/* int GetDIBits(HDC, HBITMAP, UINT start, UINT lines, void FAR *bits, BITMAPINFO FAR *, UINT usage) */
static void GetDIBits(CPU *c)
{
    uint32_t a[7];
    uint16_t n = api_args(c, "wwwwllw", a);
    Bitmap *bm = h_get((uint16_t)a[1], HT_BITMAP);
    c->a.x = 0;
    if (bm) {
        uint8_t *info = (uint8_t *)guest_ptr(a[5]);
        int bpp = rd16_host(info + 14);
        if (!bpp)
            bpp = bm->s.mono ? 1 : 8;
        int w = bm->s.w, h = bm->s.h;
        int stride = ((w * bpp + 31) / 32) * 4;
        wr32_host(info + 4, (uint32_t)w);
        wr32_host(info + 8, (uint32_t)h);
        wr16_host(info + 12, 1);
        wr16_host(info + 14, (uint16_t)bpp);
        wr32_host(info + 16, 0);
        wr32_host(info + 20, (uint32_t)(stride * h));
        int ncolors = bpp <= 8 ? 1 << bpp : 0;
        uint8_t *table = info + rd32_host(info);
        for (int i = 0; i < ncolors; i++) {
            if (a[6] == 1) {
                wr16_host(table + 2 * i, (uint16_t)i);
            } else {
                uint8_t r, g, b;
                if (bpp == 1) {
                    r = g = b = i ? 255 : 0;
                } else {
                    r = g_syspal[i][0]; g = g_syspal[i][1]; b = g_syspal[i][2];
                }
                table[4 * i] = b; table[4 * i + 1] = g; table[4 * i + 2] = r; table[4 * i + 3] = 0;
            }
        }
        if (a[4]) {
            uint8_t *bits = (uint8_t *)guest_ptr(a[4]);
            int start = (int)a[2], lines = (int)a[3];
            if (start + lines > h)
                lines = h - start;
            for (int i = 0; i < lines; i++) {
                int y = h - 1 - (start + i);
                const uint8_t *src = bm->s.bits + (size_t)y * bm->s.stride;
                uint8_t *dst = bits + (size_t)i * stride;
                memset(dst, 0, (size_t)stride);
                for (int x = 0; x < w; x++) {
                    uint8_t v = src[x];
                    switch (bpp) {
                    case 1: {
                        int on = bm->s.mono ? v != 0 : (g_syspal[v][0] * 30 + g_syspal[v][1] * 59 + g_syspal[v][2] * 11) >= 12800;
                        if (on) dst[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
                        break;
                    }
                    case 4: dst[x >> 1] |= (uint8_t)((v & 15) << ((x & 1) ? 0 : 4)); break;
                    case 8: dst[x] = bm->s.mono ? (v ? 255 : 0) : v; break;
                    case 24: {
                        const uint8_t *pc = bm->s.mono ? (v ? (const uint8_t *)"\xff\xff\xff" : (const uint8_t *)"\0\0\0") : g_syspal[v];
                        dst[3 * x] = pc[2]; dst[3 * x + 1] = pc[1]; dst[3 * x + 2] = pc[0];
                        break;
                    }
                    }
                }
            }
            c->a.x = (uint16_t)lines;
        } else {
            c->a.x = (uint16_t)h;
        }
    }
    api_ret(c, n);
}

/* HBITMAP CreateDIBitmap(HDC, const BITMAPINFOHEADER FAR *, DWORD init, const void FAR *bits,
 *                        const BITMAPINFO FAR *, UINT usage) */
static void CreateDIBitmap(CPU *c)
{
    uint32_t a[6];
    uint16_t n = api_args(c, "wllllw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int w, h, bpp, nc, hs, es;
    dib_info((const uint8_t *)guest_ptr(a[1]), &w, &h, &bpp, &nc, &hs, &es);
    uint16_t hb = gdi_new_bitmap(w, h < 0 ? -h : h, 0);
    Bitmap *bm = h_get(hb, HT_BITMAP);
    Surface s;
    LOGD("CreateDIBitmap(%s, %dx%d %d bpp, init %x, bits %04x:%04x, usage %u) = %04x", dc_desc(dc), w, h, bpp, a[2],
         FAR_SEL(a[3]), FAR_OFF(a[3]), a[5], hb);
    if (g_log_level >= LOG_DEBUG && a[3]) {
        const uint8_t *raw = (const uint8_t *)guest_ptr(a[3]);
        int nz = 0, total = w * (h < 0 ? -h : h);
        for (int i = 0; i < total; i++)
            nz += raw[i] != 0;
        LOGD("  DIB bits: %d of %d nonzero, first %02x %02x %02x %02x", nz, total, raw[0], raw[1], raw[2], raw[3]);
    }
    if (bm && (a[2] & 4) && a[3] && a[4] && dib_decode(dc, a[4], a[3], (int)a[5], 0, -1, &s)) {
        for (int y = 0; y < s.h && y < bm->s.h; y++)
            memcpy(bm->s.bits + (size_t)y * bm->s.stride, s.bits + (size_t)y * s.stride,
                   (size_t)(s.w < bm->s.w ? s.w : bm->s.w));
        free(s.bits);
    }
    c->a.x = hb;
    api_ret(c, n);
}

/* ---- device-dependent bitmaps ------------------------------------------------------------- */

static int ddb_stride(const Surface *s) { return s->mono ? ((s->w + 15) / 16) * 2 : (s->w + 1) & ~1; }

static void ddb_set(Surface *s, const uint8_t *bits, uint32_t count)
{
    int stride = ddb_stride(s);
    for (int y = 0; y < s->h; y++) {
        const uint8_t *src = bits + (size_t)y * stride;
        if ((uint32_t)(y + 1) * stride > count)
            break;
        uint8_t *dst = s->bits + (size_t)y * s->stride;
        if (s->mono)
            for (int x = 0; x < s->w; x++)
                dst[x] = (src[x >> 3] >> (7 - (x & 7)) & 1) ? 0xFF : 0x00;
        else
            memcpy(dst, src, (size_t)s->w);
    }
}

static uint32_t ddb_get(const Surface *s, uint8_t *bits, uint32_t count)
{
    int stride = ddb_stride(s);
    uint32_t done = 0;
    for (int y = 0; y < s->h; y++) {
        if ((uint32_t)(y + 1) * stride > count)
            break;
        uint8_t *dst = bits + (size_t)y * stride;
        const uint8_t *src = s->bits + (size_t)y * s->stride;
        memset(dst, 0, (size_t)stride);
        if (s->mono) {
            for (int x = 0; x < s->w; x++)
                if (src[x])
                    dst[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        } else {
            memcpy(dst, src, (size_t)s->w);
        }
        done += (uint32_t)stride;
    }
    return done;
}

/* HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void FAR *bits) */
static uint16_t create_bitmap(int w, int h, int planes, int bpp, uint32_t bits)
{
    int mono = planes * bpp == 1;
    if (!mono && bpp != 8)
        LOGW("CreateBitmap with %d planes x %d bpp: treated as 8 bpp", planes, bpp);
    uint16_t hb = gdi_new_bitmap(w, h, mono);
    Bitmap *bm = h_get(hb, HT_BITMAP);
    if (bm && bits)
        ddb_set(&bm->s, (const uint8_t *)guest_ptr(bits), 0xFFFFFFFF);
    return hb;
}

static void CreateBitmap(CPU *c)
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwwl", a);
    c->a.x = create_bitmap((int16_t)a[0], (int16_t)a[1], (int)a[2], (int)a[3], a[4]);
    api_ret(c, n);
}

static void CreateBitmapIndirect(CPU *c)   /* HBITMAP CreateBitmapIndirect(const BITMAP FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    c->a.x = create_bitmap((int16_t)rd16(s, o + 2), (int16_t)rd16(s, o + 4), rd8(s, o + 8), rd8(s, o + 9),
                           rd32(s, o + 10));
    api_ret(c, n);
}

/* HBITMAP CreateCompatibleBitmap(HDC, int, int) */
static void CreateCompatibleBitmap(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int w = (int16_t)a[1], h = (int16_t)a[2];
    int mono = 0;
    if (dc && dc->kind == DC_MEMORY) {
        Bitmap *bm = h_get(dc->hbitmap, HT_BITMAP);
        mono = bm && bm->s.mono;
    }
    c->a.x = (w <= 0 || h <= 0) ? gdi_new_bitmap(1, 1, 1) : gdi_new_bitmap(w, h, mono);
    api_ret(c, n);
}

static void GetBitmapBits(CPU *c)      /* LONG GetBitmapBits(HBITMAP, LONG count, void FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wll", a);
    Bitmap *bm = h_get((uint16_t)a[0], HT_BITMAP);
    api_ret32(c, n, bm ? ddb_get(&bm->s, (uint8_t *)guest_ptr(a[2]), a[1]) : 0);
}

static void SetBitmapBits(CPU *c)      /* LONG SetBitmapBits(HBITMAP, DWORD count, const void FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wll", a);
    Bitmap *bm = h_get((uint16_t)a[0], HT_BITMAP);
    if (bm) {
        ddb_set(&bm->s, (const uint8_t *)guest_ptr(a[2]), a[1]);
        if (bm->dc)
            video_dirty();
    }
    api_ret32(c, n, bm ? a[1] : 0);
}

const ApiDef g_gdi_draw_api[] = {
    { "GDI", 19, "LineTo", LineTo },
    { "GDI", 24, "Ellipse", Ellipse },
    { "GDI", 27, "Rectangle", Rectangle },
    { "GDI", 28, "RoundRect", RoundRect },
    { "GDI", 29, "PatBlt", PatBlt },
    { "GDI", 31, "SetPixel", SetPixel },
    { "GDI", 34, "BitBlt", BitBlt },
    { "GDI", 35, "StretchBlt", StretchBlt },
    { "GDI", 36, "Polygon", Polygon },
    { "GDI", 37, "Polyline", Polyline },
    { "GDI", 40, "FillRgn", FillRgn },
    { "GDI", 41, "FrameRgn", FrameRgn },
    { "GDI", 42, "InvertRgn", InvertRgn },
    { "GDI", 43, "PaintRgn", PaintRgn },
    { "GDI", 48, "CreateBitmap", CreateBitmap },
    { "GDI", 49, "CreateBitmapIndirect", CreateBitmapIndirect },
    { "GDI", 51, "CreateCompatibleBitmap", CreateCompatibleBitmap },
    { "GDI", 74, "GetBitmapBits", GetBitmapBits },
    { "GDI", 83, "GetPixel", GetPixel },
    { "GDI", 106, "SetBitmapBits", SetBitmapBits },
    { "GDI", 439, "StretchDIBits", StretchDIBits },
    { "GDI", 440, "SetDIBits", SetDIBits },
    { "GDI", 441, "GetDIBits", GetDIBits },
    { "GDI", 442, "CreateDIBitmap", CreateDIBitmap },
    { "GDI", 443, "SetDIBitsToDevice", SetDIBitsToDevice },
    { "GDI", 450, "PolyPolygon", PolyPolygon },
    { "USER", 81, "FillRect", FillRect },
    { "USER", 82, "InvertRect", InvertRect },
    { "USER", 83, "FrameRect", FrameRect },
    { "USER", 221, "ScrollDC", ScrollDC },
    { "USER", 466, "DrawFocusRect", DrawFocusRect },
    { NULL, 0, NULL, NULL }
};
