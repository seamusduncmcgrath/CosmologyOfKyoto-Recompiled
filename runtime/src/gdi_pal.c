/* Palettes and colour matching on the 8-bit palettized screen.
 *
 * The hardware palette g_syspal has 20 static colours (indices 0-9 and 246-255) unless the
 * program asks for SYSPAL_NOSTATIC (then only 0 = black and 255 = white stay). Realizing a
 * logical palette in the foreground assigns its entries to the free system entries in
 * order, collapsing entries onto identical colours unless PC_NOCOLLAPSE/PC_RESERVED is set,
 * which is the behaviour "identity palette" programs such as Director rely on. Pixels on
 * screen and in bitmaps are system indices, so a palette change recolours the screen.
 */
#include <string.h>

#include "win.h"

static const uint8_t g_static_rgb[20][3] = {
    { 0, 0, 0 },       { 128, 0, 0 },     { 0, 128, 0 },     { 128, 128, 0 },   { 0, 0, 128 },
    { 128, 0, 128 },   { 0, 128, 128 },   { 192, 192, 192 }, { 192, 220, 192 }, { 166, 202, 240 },
    { 255, 251, 240 }, { 160, 160, 164 }, { 128, 128, 128 }, { 255, 0, 0 },     { 0, 255, 0 },
    { 255, 255, 0 },   { 0, 0, 255 },     { 255, 0, 255 },   { 0, 255, 255 },   { 255, 255, 255 },
};

uint16_t g_default_palette;
unsigned g_syspal_gen = 1;
static int g_nostatic;                       /* SYSPAL_NOSTATIC in effect */

static int static_index(int i) { return i < 10 ? i : 236 + i; }       /* 0..19 -> system index */
static int is_static(int sys) { return g_nostatic ? (sys == 0 || sys == 255) : (sys < 10 || sys >= 246); }

void pal_init(void)
{
    memset(g_syspal, 0, sizeof(g_syspal));
    for (int i = 0; i < 20; i++)
        memcpy(g_syspal[static_index(i)], g_static_rgb[i], 3);
    Palette *p = calloc(1, sizeof(Palette));
    p->n = 20;
    for (int i = 0; i < 20; i++) {
        memcpy(p->e[i], g_static_rgb[i], 3);
        p->map[i] = (uint8_t)static_index(i);
    }
    g_default_palette = h_alloc(HT_PALETTE, p);
}

static int dist(const uint8_t *a, int r, int g, int b)
{
    int dr = a[0] - r, dg = a[1] - g, db = a[2] - b;
    return dr * dr * 30 + dg * dg * 59 + db * db * 11;
}

int pal_nearest(const Palette *p, int r, int g, int b)
{
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < p->n; i++) {
        int d = dist(p->e[i], r, g, b);
        if (d < bd) {
            bd = d;
            best = i;
            if (!d)
                break;
        }
    }
    return best;
}

int syspal_nearest_static(int r, int g, int b)
{
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < 256; i++) {
        if (!is_static(i))
            continue;
        int d = dist(g_syspal[i], r, g, b);
        if (d < bd) {
            bd = d;
            best = i;
            if (!d)
                break;
        }
    }
    return best;
}

static int syspal_nearest(int r, int g, int b)
{
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < 256; i++) {
        int d = dist(g_syspal[i], r, g, b);
        if (d < bd) {
            bd = d;
            best = i;
            if (!d)
                break;
        }
    }
    return best;
}

/* An unrealized palette maps to the nearest current system colours. */
static void map_background(Palette *p)
{
    for (int i = 0; i < p->n; i++)
        p->map[i] = (uint8_t)syspal_nearest(p->e[i][0], p->e[i][1], p->e[i][2]);
}

/* Foreground realization; returns the number of system entries that changed. */
static int realize(Palette *p)
{
    if (h_get(g_default_palette, HT_PALETTE) == p)
        return 0;
    uint8_t used[256];
    for (int i = 0; i < 256; i++)
        used[i] = (uint8_t)is_static(i);
    int next = 0, changed = 0;
    for (int i = 0; i < p->n; i++) {
        int r = p->e[i][0], g = p->e[i][1], b = p->e[i][2], f = p->e[i][3];
        if (f & PC_EXPLICIT) {
            p->map[i] = (uint8_t)r;
            continue;
        }
        if (!(f & (PC_RESERVED | PC_NOCOLLAPSE))) {
            int found = -1;
            for (int s = 0; s < 256 && found < 0; s++)
                if (used[s] && g_syspal[s][0] == r && g_syspal[s][1] == g && g_syspal[s][2] == b)
                    found = s;
            if (found >= 0) {
                p->map[i] = (uint8_t)found;
                continue;
            }
        }
        while (next < 256 && used[next])
            next++;
        if (next < 256) {
            if (g_syspal[next][0] != r || g_syspal[next][1] != g || g_syspal[next][2] != b) {
                g_syspal[next][0] = (uint8_t)r;
                g_syspal[next][1] = (uint8_t)g;
                g_syspal[next][2] = (uint8_t)b;
                changed++;
            }
            used[next] = 1;
            p->map[i] = (uint8_t)next;
        } else {
            int best = 0, bd = 0x7FFFFFFF;
            for (int s = 0; s < 256; s++)
                if (used[s] && dist(g_syspal[s], r, g, b) < bd) {
                    bd = dist(g_syspal[s], r, g, b);
                    best = s;
                }
            p->map[i] = (uint8_t)best;
        }
    }
    if (changed) {
        g_syspal_gen++;
        video_dirty();
    }
    return changed;
}

/* ---- DIB device contexts: maps between the DIB colour table and the system palette ---- */

static const uint8_t *dib_table(DC *dc, int *n)
{
    const uint8_t *info = (const uint8_t *)guest_ptr(dc->dib_info);
    uint32_t size = rd32_host(info);
    uint32_t used = size >= 36 ? rd32_host(info + 32) : 0;
    int full = dc->dib_bpp && dc->dib_bpp < 8 ? 1 << dc->dib_bpp : 256;
    *n = used && used <= (uint32_t)full ? (int)used : full;
    return info + size;
}

int dib_nearest(DC *dc, int r, int g, int b)
{
    int n;
    const uint8_t *t = dib_table(dc, &n);
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        int dr = t[4 * i + 2] - r, dg = t[4 * i + 1] - g, db = t[4 * i] - b;
        int d = dr * dr * 30 + dg * dg * 59 + db * db * 11;
        if (d < bd) {
            bd = d;
            best = i;
            if (!d)
                break;
        }
    }
    return best;
}

/* Bring a DIB DC's surface maps up to date with its colour table and the system palette. */
void dib_refresh_maps(DC *dc)
{
    if (dc->dib.mono) {                           /* bits, not colour indices: no maps */
        dc->dib.map = dc->dib.inv = NULL;
        return;
    }
    int n;
    const uint8_t *t = dib_table(dc, &n);
    uint32_t sig = 2166136261u;
    for (int i = 0; i < n * 4; i++)
        sig = (sig ^ t[i]) * 16777619u;
    if (sig == dc->dib_sig && dc->dib_gen == g_syspal_gen)
        return;
    dc->dib_sig = sig;
    dc->dib_gen = g_syspal_gen;
    int identity = n == 256;
    for (int i = 0; i < 256; i++) {
        int s = i < n ? syspal_nearest(t[4 * i + 2], t[4 * i + 1], t[4 * i]) : 0;
        dc->dib_map[i] = (uint8_t)s;
        if (s != i)
            identity = 0;
    }
    for (int s = 0; s < 256; s++) {
        int v = dib_nearest(dc, g_syspal[s][0], g_syspal[s][1], g_syspal[s][2]);
        dc->dib_inv[s] = (uint8_t)v;
        if (v != s)
            identity = 0;
    }
    dc->dib.map = identity ? NULL : dc->dib_map;
    dc->dib.inv = identity ? NULL : dc->dib_inv;
}

static Palette *dc_palette(DC *dc)
{
    Palette *p = dc ? h_get(dc->hpalette, HT_PALETTE) : NULL;
    return p ? p : h_get(g_default_palette, HT_PALETTE);
}

uint8_t dc_color(DC *dc, uint32_t cref)
{
    Palette *p = dc_palette(dc);
    int type = (int)(cref >> 24);
    int r = cref & 0xFF, g = (cref >> 8) & 0xFF, b = (cref >> 16) & 0xFF;
    uint8_t sys;
    if ((cref >> 16) == 0x10FF) {                 /* DIBINDEX(n): entry n of a DIB device's table */
        int idx = cref & 0xFF;
        if (dc && dc->kind == DC_DIB)
            return dc->dib.mono ? (idx & 1 ? 0xFF : 0x00) : (uint8_t)idx;
        type = 1;                                 /* elsewhere: as a palette index */
        cref = 0x01000000u | (uint32_t)idx;
    }
    if (type == 1) {
        int idx = cref & 0xFFFF;
        if (idx >= p->n)
            idx = 0;
        sys = p->map[idx];
        r = p->e[idx][0];
        g = p->e[idx][1];
        b = p->e[idx][2];
    } else if (type == 2) {
        sys = p->map[pal_nearest(p, r, g, b)];
    } else {
        sys = (uint8_t)syspal_nearest_static(r, g, b);
    }
    Bitmap *bm = dc && dc->kind == DC_MEMORY ? h_get(dc->hbitmap, HT_BITMAP) : NULL;
    if (bm && bm->s.mono)
        return (r * 30 + g * 59 + b * 11) >= 128 * 100 ? 0xFF : 0x00;
    if (dc && dc->kind == DC_DIB) {               /* nearest colour of the DIB's table */
        if (type == 0) {
            r = g_syspal[sys][0];
            g = g_syspal[sys][1];
            b = g_syspal[sys][2];
        }
        int idx = dib_nearest(dc, r, g, b);
        return dc->dib.mono ? (idx ? 0xFF : 0x00) : (uint8_t)idx;
    }
    return sys;
}

/* Translation table for DIB colour indices into system indices, for a DC's palette. */
void dib_color_map(DC *dc, const uint8_t *table, int ncolors, int entry_size, int pal_colors, int mono_dst,
                   uint8_t out[256])
{
    Palette *p = dc_palette(dc);
    memset(out, 0, 256);
    for (int i = 0; i < ncolors && i < 256; i++) {
        const uint8_t *e = table + i * entry_size;
        int r, g, b;
        if (pal_colors) {
            int idx = e[0] | e[1] << 8;
            if (idx >= p->n)
                idx = 0;
            out[i] = p->map[idx];
            r = p->e[idx][0]; g = p->e[idx][1]; b = p->e[idx][2];
        } else {
            b = e[0]; g = e[1]; r = e[2];             /* RGBQUAD / RGBTRIPLE are blue first */
            out[i] = p->map[pal_nearest(p, r, g, b)];
        }
        if (mono_dst)
            out[i] = (r * 30 + g * 59 + b * 11) >= 128 * 100 ? 0xFF : 0x00;
    }
}

uint8_t dib_rgb_to_sys(DC *dc, int r, int g, int b)
{
    Palette *p = dc_palette(dc);
    return p->map[pal_nearest(p, r, g, b)];
}

/* ---- the API -------------------------------------------------------------------------------- */

static void CreatePalette(CPU *c)      /* HPALETTE CreatePalette(const LOGPALETTE FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    Palette *p = calloc(1, sizeof(Palette));
    p->n = rd16(s, o + 2);
    if (p->n > 256)
        p->n = 256;
    memcpy(p->e, mem_ptr(s, o + 4), (size_t)p->n * 4);
    map_background(p);
    c->a.x = h_alloc(HT_PALETTE, p);
    LOGD("CreatePalette(%d entries) = %04x", p->n, c->a.x);
    api_ret(c, n);
}

/* UINT SetPaletteEntries(HPALETTE, UINT start, UINT n, const PALETTEENTRY FAR *) */
static void SetPaletteEntries(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    Palette *p = h_get((uint16_t)a[0], HT_PALETTE);
    c->a.x = 0;
    if (p && a[1] < (uint32_t)p->n) {
        uint32_t cnt = a[2];
        if (a[1] + cnt > (uint32_t)p->n)
            cnt = (uint32_t)p->n - a[1];
        memcpy(p->e[a[1]], guest_ptr(a[3]), cnt * 4);
        c->a.x = (uint16_t)cnt;
    }
    api_ret(c, n);
}

static void GetPaletteEntries(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    Palette *p = h_get((uint16_t)a[0], HT_PALETTE);
    c->a.x = 0;
    if (p && a[1] < (uint32_t)p->n) {
        uint32_t cnt = a[2];
        if (a[1] + cnt > (uint32_t)p->n)
            cnt = (uint32_t)p->n - a[1];
        memcpy(guest_ptr(a[3]), p->e[a[1]], cnt * 4);
        c->a.x = (uint16_t)cnt;
    }
    api_ret(c, n);
}

static void ResizePalette(CPU *c)      /* BOOL ResizePalette(HPALETTE, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Palette *p = h_get((uint16_t)a[0], HT_PALETTE);
    c->a.x = 0;
    if (p && a[1] <= 256) {
        for (uint32_t i = (uint32_t)p->n; i < a[1]; i++)
            memset(p->e[i], 0, 4);
        p->n = (int)a[1];
        c->a.x = 1;
    }
    api_ret(c, n);
}

/* void AnimatePalette(HPALETTE, UINT start, UINT n, const PALETTEENTRY FAR *) */
static void AnimatePalette(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    Palette *p = h_get((uint16_t)a[0], HT_PALETTE);
    if (p) {
        const uint8_t *src = (const uint8_t *)guest_ptr(a[3]);
        for (uint32_t i = 0; i < a[2] && a[1] + i < (uint32_t)p->n; i++) {
            uint8_t *e = p->e[a[1] + i];
            if (!(e[3] & PC_RESERVED))
                continue;
            memcpy(e, src + i * 4, 3);
            memcpy(g_syspal[p->map[a[1] + i]], e, 3);
        }
        g_syspal_gen++;
        video_dirty();
    }
    api_ret(c, n);
}

static void GetNearestPaletteIndex(CPU *c)     /* UINT GetNearestPaletteIndex(HPALETTE, COLORREF) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Palette *p = h_get((uint16_t)a[0], HT_PALETTE);
    c->a.x = p ? (uint16_t)pal_nearest(p, a[1] & 0xFF, (a[1] >> 8) & 0xFF, (a[1] >> 16) & 0xFF) : 0;
    api_ret(c, n);
}

static void GetNearestColor(CPU *c)    /* COLORREF GetNearestColor(HDC, COLORREF) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    uint8_t sys = dc_color(dc_get((uint16_t)a[0]), a[1]);
    api_ret32(c, n, (uint32_t)g_syspal[sys][0] | (uint32_t)g_syspal[sys][1] << 8 | (uint32_t)g_syspal[sys][2] << 16);
}

/* UINT GetSystemPaletteEntries(HDC, UINT start, UINT n, PALETTEENTRY FAR *) */
static void GetSystemPaletteEntries(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    uint8_t *out = (uint8_t *)guest_ptr(a[3]);
    uint32_t cnt = 0;
    for (uint32_t i = a[1]; i < 256 && cnt < a[2]; i++, cnt++) {
        memcpy(out + cnt * 4, g_syspal[i], 3);
        out[cnt * 4 + 3] = 0;
    }
    c->a.x = (uint16_t)cnt;
    api_ret(c, n);
}

static void SetSystemPaletteUse(CPU *c)    /* UINT SetSystemPaletteUse(HDC, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = g_nostatic ? 2 : 1;
    int want = a[1] == 2;
    if (want != g_nostatic) {
        g_nostatic = want;
        if (!g_nostatic)                           /* static colours come back */
            for (int i = 0; i < 20; i++)
                memcpy(g_syspal[static_index(i)], g_static_rgb[i], 3);
        g_syspal_gen++;
        video_dirty();
    }
    LOGD("SetSystemPaletteUse(%s)", want ? "NOSTATIC" : "STATIC");
    api_ret(c, n);
}

static void GetSystemPaletteUse(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = g_nostatic ? 2 : 1;
    api_ret(c, n);
}

/* USER SelectPalette(HDC, HPALETTE, BOOL background): previous palette */
static void SelectPalette(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    if (dc && h_get((uint16_t)a[1], HT_PALETTE)) {
        c->a.x = dc->hpalette ? dc->hpalette : g_default_palette;
        dc->hpalette = (uint16_t)a[1];
    }
    api_ret(c, n);
}

/* USER RealizePalette(HDC): number of entries mapped to new system colours */
static void RealizePalette(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int changed = 0;
    if (dc) {
        changed = realize(dc_palette(dc));
        if (changed) {
            uint16_t from = dc->hwnd ? dc->hwnd : g_active;
            Wnd *d = wnd_get(g_desktop);
            for (uint16_t h = d->child; h; ) {
                Wnd *w = wnd_get(h);
                uint16_t next = w->next;
                wnd_send(h, WM_PALETTECHANGED, from, 0);
                h = next;
            }
        }
    }
    LOGD("RealizePalette(%04x) = %d", a[0], changed);
    c->a.x = (uint16_t)changed;
    api_ret(c, n);
}

static void UnrealizeObject(CPU *c)    /* BOOL UnrealizeObject(HGDIOBJ) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 1;
    api_ret(c, n);
}

static void UpdateColors(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 1;
    api_ret(c, n);
}

const ApiDef g_gdi_pal_api[] = {
    { "GDI", 150, "UnrealizeObject", UnrealizeObject },
    { "GDI", 154, "GetNearestColor", GetNearestColor },
    { "GDI", 360, "CreatePalette", CreatePalette },
    { "GDI", 363, "GetPaletteEntries", GetPaletteEntries },
    { "GDI", 364, "SetPaletteEntries", SetPaletteEntries },
    { "GDI", 366, "UpdateColors", UpdateColors },
    { "GDI", 367, "AnimatePalette", AnimatePalette },
    { "GDI", 368, "ResizePalette", ResizePalette },
    { "GDI", 370, "GetNearestPaletteIndex", GetNearestPaletteIndex },
    { "GDI", 373, "SetSystemPaletteUse", SetSystemPaletteUse },
    { "GDI", 374, "GetSystemPaletteUse", GetSystemPaletteUse },
    { "GDI", 375, "GetSystemPaletteEntries", GetSystemPaletteEntries },
    { "USER", 282, "SelectPalette", SelectPalette },
    { "USER", 283, "RealizePalette", RealizePalette },
    { NULL, 0, NULL, NULL }
};
