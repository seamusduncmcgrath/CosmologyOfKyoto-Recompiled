/* GDI objects and device contexts: pens, brushes, bitmaps, regions, stock objects, DC
 * attributes, coordinate mapping and clipping.
 *
 * Only MM_TEXT mapping is supported (device = logical - window origin + viewport origin).
 * A DC's drawing target (surface, offset, clip region in surface coordinates) is worked
 * out lazily and cached until the clip or the window layout changes (dc_target).
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "gdi.h"

/* ---- stock objects --------------------------------------------------------------------------- */

static uint16_t g_stock_obj[17];
static uint16_t g_stock_bitmap;              /* 1x1 monochrome, the default for memory DCs */

uint16_t gdi_stock(int i) { return i >= 0 && i < 17 ? g_stock_obj[i] : 0; }

static int is_stock(uint16_t h)
{
    if (!h)
        return 1;
    for (int i = 0; i < 17; i++)
        if (g_stock_obj[i] == h)
            return 1;
    return h == g_stock_bitmap;
}

static uint16_t new_pen(int style, int width, uint32_t color)
{
    Pen *p = calloc(1, sizeof(Pen));
    p->style = style;
    p->width = width;
    p->color = color;
    return h_alloc(HT_PEN, p);
}

static uint16_t new_brush(int style, uint32_t color, int hatch)
{
    Brush *b = calloc(1, sizeof(Brush));
    b->style = style;
    b->color = color;
    b->hatch = hatch;
    return h_alloc(HT_BRUSH, b);
}

uint16_t gdi_solid_brush(uint32_t color)
{
    static struct { uint32_t color; uint16_t h; } cache[32];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].color == color)
            return cache[i].h;
    uint16_t h = new_brush(BS_SOLID, color, 0);
    if (n < 32) {
        cache[n].color = color;
        cache[n++].h = h;
    }
    return h;
}

int surface_init(Surface *s, int w, int h, int mono)
{
    s->w = w > 0 ? w : 1;
    s->h = h > 0 ? h : 1;
    s->mono = mono;
    s->map = s->inv = NULL;
    s->stride = s->w;
    s->bits = calloc((size_t)s->stride * (size_t)s->h, 1);
    if (mono)
        memset(s->bits, 0, (size_t)s->stride * (size_t)s->h);    /* black */
    return s->bits != NULL;
}

uint16_t gdi_new_bitmap(int w, int h, int mono)
{
    Bitmap *b = calloc(1, sizeof(Bitmap));
    if (!surface_init(&b->s, w, h, mono)) {
        free(b);
        return 0;
    }
    return h_alloc(HT_BITMAP, b);
}

void gdi_init(void)
{
    pal_init();
    static const uint32_t brush_colors[5] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };
    for (int i = 0; i < 5; i++)
        g_stock_obj[i] = new_brush(BS_SOLID, brush_colors[i], 0);
    g_stock_obj[5] = new_brush(BS_NULL, 0, 0);
    g_stock_obj[6] = new_pen(0, 0, 0xFFFFFF);
    g_stock_obj[7] = new_pen(0, 0, 0x000000);
    g_stock_obj[8] = new_pen(5 /* PS_NULL */, 0, 0);
    for (int i = 10; i <= 14; i++)
        g_stock_obj[i] = gdi_stock_font(i);
    g_stock_obj[16] = gdi_stock_font(16);
    g_stock_obj[15] = g_default_palette;
    g_stock_bitmap = gdi_new_bitmap(1, 1, 1);
}

/* ---- DCs --------------------------------------------------------------------------------- */

static DC *g_last_big_dib;
static DC *dc_new(int kind)
{
    DC *dc = calloc(1, sizeof(DC));
    dc->kind = kind;
    dc->text_color = 0x000000;
    dc->bk_color = 0xFFFFFF;
    dc->bk_mode = 2;                             /* OPAQUE */
    dc->rop2 = 13;                               /* R2_COPYPEN */
    dc->stretch_mode = 1;
    dc->poly_fill_mode = 1;
    dc->map_mode = 1;
    dc->hpen = g_stock_obj[7];
    dc->hbrush = g_stock_obj[0];
    dc->hfont = g_stock_obj[13];
    dc->hpalette = g_default_palette;
    rgn_init(&dc->paint_rgn);
    rgn_init(&dc->clip);
    rgn_init(&dc->cache_clip);
    dc->cache_gen = 0;
    return dc;
}

DC *dc_get(uint16_t hdc) { return h_get(hdc, HT_DC); }

static void dc_invalidate(DC *dc) { dc->cache_gen = 0; }

/* ---- 1 bpp DIB devices -------------------------------------------------------------------
 *
 * Surfaces are byte-per-pixel, so a monochrome DIB device draws into a shadow of the
 * guest's bits: 0x00 for a 0 bit and 0xFF for a 1 bit, like a monochrome bitmap, so raster
 * operations work bitwise on it. The program owns the bits and reads and writes them
 * itself, so the shadow is loaded the first time an API call draws with the DC (dc_target)
 * and stored back when that call returns (gdi_flush_dib_shadows, from the dispatcher).
 * Director makes its sprite masks this way: matte ink and clicks on matte sprites use them. */
#define MAX_LIVE_SHADOWS 16
static DC *g_live_shadow[MAX_LIVE_SHADOWS];
static int g_live_shadows;
int g_dib_shadows_pending;

static uint8_t *dib_guest_row(DC *dc, int y)
{
    const uint8_t *info = (const uint8_t *)guest_ptr(dc->dib_info);
    int w, h, bpp, ncolors, hsize, esize;
    dib_info(info, &w, &h, &bpp, &ncolors, &hsize, &esize);
    int row = ((w * bpp + 31) / 32) * 4, ah = h < 0 ? -h : h;
    uint8_t *bits = (uint8_t *)info + hsize + ncolors * esize;
    return bits + (size_t)(h < 0 ? y : ah - 1 - y) * row;
}

static void shadow_load(DC *dc)
{
    for (int y = 0; y < dc->dib.h; y++) {
        const uint8_t *src = dib_guest_row(dc, y);
        uint8_t *dst = surf_row(&dc->dib, y);
        for (int x = 0; x < dc->dib.w; x++)
            dst[x] = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 0xFF : 0x00;
    }
}

static void shadow_store(DC *dc)
{
    for (int y = 0; y < dc->dib.h; y++) {
        uint8_t *dst = dib_guest_row(dc, y);
        const uint8_t *src = surf_row(&dc->dib, y);
        for (int x = 0; x < dc->dib.w; x++) {
            uint8_t bit = (uint8_t)(0x80 >> (x & 7));
            dst[x >> 3] = src[x] & 1 ? dst[x >> 3] | bit : dst[x >> 3] & ~bit;
        }
    }
}

void gdi_flush_dib_shadows(void)
{
    for (int i = 0; i < g_live_shadows; i++) {
        shadow_store(g_live_shadow[i]);
        g_live_shadow[i]->shadow_live = 0;
    }
    g_live_shadows = 0;
    g_dib_shadows_pending = 0;
}

static void shadow_use(DC *dc)
{
    if (dc->shadow_live)
        return;
    if (g_live_shadows == MAX_LIVE_SHADOWS)
        gdi_flush_dib_shadows();
    shadow_load(dc);
    dc->shadow_live = 1;
    g_live_shadow[g_live_shadows++] = dc;
    g_dib_shadows_pending = 1;
}

static void shadow_drop(DC *dc)
{
    for (int i = 0; i < g_live_shadows; i++)
        if (g_live_shadow[i] == dc) {
            shadow_store(dc);
            g_live_shadow[i] = g_live_shadow[--g_live_shadows];
            break;
        }
    free(dc->dib_shadow);
    dc->dib_shadow = NULL;
}

static void dc_free(uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (!dc)
        return;
    if (dc->dib_shadow)
        shadow_drop(dc);
    if (dc == g_last_big_dib)
        g_last_big_dib = NULL;
    Bitmap *bm = h_get(dc->hbitmap, HT_BITMAP);
    if (bm && bm->dc == hdc)
        bm->dc = 0;
    while (dc->saved) {
        DC *s = dc->saved;
        dc->saved = s->saved;
        rgn_free(&s->clip);
        free(s);
    }
    rgn_free(&dc->paint_rgn);
    rgn_free(&dc->clip);
    rgn_free(&dc->cache_clip);
    free(dc);
    h_free(hdc);
}

uint16_t gdi_screen_dc(uint16_t hwnd, int whole)
{
    Wnd *w = wnd_get(hwnd);
    if (w && !whole && (w->cls->style & CS_OWNDC)) {
        if (!dc_get(w->owndc)) {
            DC *dc = dc_new(DC_SCREEN);
            dc->hwnd = hwnd;
            dc->owned = 1;
            w->owndc = h_alloc(HT_DC, dc);
        }
        return w->owndc;
    }
    DC *dc = dc_new(DC_SCREEN);
    dc->hwnd = w ? hwnd : 0;
    dc->whole_window = whole;
    return h_alloc(HT_DC, dc);
}

void gdi_release_dc(uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (!dc)
        return;
    if (dc->owned) {
        dc->paint = 0;
        rgn_set_rect(&dc->paint_rgn, irect(0, 0, 0, 0));
        dc_invalidate(dc);
        return;
    }
    if (dc->kind == DC_SCREEN)
        dc_free(hdc);
}

void gdi_free_window_dc(uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (dc && dc->owned)
        dc_free(hdc);
}

int dc_target(DC *dc, Target *t)
{
    int vx = dc->vp_x - dc->win_x, vy = dc->vp_y - dc->win_y;
    t->dc = dc;
    if (dc->kind == DC_INFO)
        return 0;
    if (dc->kind == DC_DIB) {
        if (dc->dib_shadow)
            shadow_use(dc);
        dib_refresh_maps(dc);
        t->s = &dc->dib;
        t->ox = vx;
        t->oy = vy;
        if (dc->cache_gen != 1) {
            rgn_set_rect(&dc->cache_clip, irect(0, 0, dc->dib.w, dc->dib.h));
            if (dc->has_clip)
                rgn_combine(&dc->cache_clip, &dc->cache_clip, &dc->clip, RGN_AND);
            dc->cache_gen = 1;
        }
        t->clip = &dc->cache_clip;
        return dc->cache_clip.n > 0;
    }
    if (dc->kind == DC_MEMORY) {
        Bitmap *bm = h_get(dc->hbitmap, HT_BITMAP);
        if (!bm)
            return 0;
        t->s = &bm->s;
        t->ox = vx;
        t->oy = vy;
        if (dc->cache_gen != 1) {
            rgn_set_rect(&dc->cache_clip, irect(0, 0, bm->s.w, bm->s.h));
            if (dc->has_clip)
                rgn_combine(&dc->cache_clip, &dc->cache_clip, &dc->clip, RGN_AND);
            dc->cache_gen = 1;
        }
        t->clip = &dc->cache_clip;
        return dc->cache_clip.n > 0;
    }
    Wnd *w = dc->hwnd ? wnd_get(dc->hwnd) : NULL;
    if (dc->hwnd && !w)
        return 0;
    IRect org = w ? (dc->whole_window ? wnd_screen_rect(w) : wnd_screen_client(w)) : irect(0, 0, SCREEN_W, SCREEN_H);
    t->s = &g_screen;
    t->ox = org.l + vx;
    t->oy = org.t + vy;
    if (dc->cache_gen != g_layout_gen + 1) {
        if (w)
            wnd_vis_region(w, dc->whole_window, &dc->cache_clip);
        else
            rgn_set_rect(&dc->cache_clip, irect(0, 0, SCREEN_W, SCREEN_H));
        if (dc->paint)
            rgn_combine(&dc->cache_clip, &dc->cache_clip, &dc->paint_rgn, RGN_AND);
        if (dc->has_clip) {
            Region tmp;
            rgn_init(&tmp);
            rgn_copy(&tmp, &dc->clip);
            rgn_offset(&tmp, org.l, org.t);
            rgn_combine(&dc->cache_clip, &dc->cache_clip, &tmp, RGN_AND);
            rgn_free(&tmp);
        }
        dc->cache_gen = g_layout_gen + 1;
    }
    t->clip = &dc->cache_clip;
    return dc->cache_clip.n > 0;
}

/* screen offset of device coordinate 0,0 (not including the logical mapping) */
static IRect dc_origin(DC *dc)
{
    if (dc->kind == DC_SCREEN && dc->hwnd) {
        Wnd *w = wnd_get(dc->hwnd);
        if (w)
            return dc->whole_window ? wnd_screen_rect(w) : wnd_screen_client(w);
    }
    return irect(0, 0, 0, 0);
}

/* the device-coordinate area a DC can draw into, before its clip region */
static IRect dc_extent(DC *dc)
{
    if (dc->kind == DC_MEMORY) {
        Bitmap *bm = h_get(dc->hbitmap, HT_BITMAP);
        return bm ? irect(0, 0, bm->s.w, bm->s.h) : irect(0, 0, 0, 0);
    }
    if (dc->kind == DC_DIB)
        return irect(0, 0, dc->dib.w, dc->dib.h);
    if (dc->kind == DC_SCREEN && dc->hwnd) {
        Wnd *w = wnd_get(dc->hwnd);
        if (w) {
            IRect r = dc->whole_window ? wnd_screen_rect(w) : wnd_screen_client(w);
            return irect(0, 0, r.r - r.l, r.b - r.t);
        }
    }
    return irect(0, 0, SCREEN_W, SCREEN_H);
}

/* ---- API: DCs ------------------------------------------------------------------------------- */

static void GetDC(CPU *c)              /* HDC GetDC(HWND) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = gdi_screen_dc((uint16_t)a[0], 0);
    api_ret(c, n);
}

static void GetWindowDC(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = gdi_screen_dc((uint16_t)a[0], 1);
    api_ret(c, n);
}

static void ReleaseDC(CPU *c)          /* int ReleaseDC(HWND, HDC) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    gdi_release_dc((uint16_t)a[1]);
    c->a.x = 1;
    api_ret(c, n);
}

/* A DIB device: draws into the packed DIB (BITMAPINFOHEADER, colour table, bits) at `info`,
 * which stays in guest memory so the program can also work on the bits directly. */
static uint16_t create_dib_dc(uint32_t info_far)
{
    const uint8_t *info = (const uint8_t *)guest_ptr(info_far);
    int w, h, bpp, ncolors, hsize, esize;
    int comp = dib_info(info, &w, &h, &bpp, &ncolors, &hsize, &esize);
    if ((bpp != 8 && bpp != 1) || comp != 0 || esize != 4) {
        LOGW("DIB device: %d bpp, compression %d not supported", bpp, comp);
        return 0;
    }
    int ah = h < 0 ? -h : h;
    int row = ((w * bpp + 31) / 32) * 4;
    uint8_t *bits = (uint8_t *)info + hsize + ncolors * 4;
    DC *dc = dc_new(DC_DIB);
    dc->dib_info = info_far;
    dc->dib_bpp = bpp;
    dc->dib.w = w;
    dc->dib.h = ah;
    if (bpp == 1) {                               /* a shadow, loaded when drawn into */
        dc->dib_shadow = calloc(1, (size_t)w * ah + 1);
        dc->dib.mono = 1;
        dc->dib.stride = w;
        dc->dib.bits = dc->dib_shadow;
    } else {
        dc->dib.stride = h < 0 ? row : -row;
        dc->dib.bits = h < 0 ? bits : bits + (size_t)(ah - 1) * row;
    }
    dc->dib_gen = 0;
    dib_refresh_maps(dc);
    uint16_t hdc = h_alloc(HT_DC, dc);
    if (w * ah >= 320 * 200) {
        g_last_big_dib = dc;
        if (SDL_getenv("COK_WATCH_DIB")) {        /* debugging: who writes the pixels? */
            g_watch_lo = bits;
            g_watch_hi = bits + (size_t)ah * row;
            LOGI("watching DIB pixels %p-%p", (void *)g_watch_lo, (void *)g_watch_hi);
        }
    }
    LOGD("CreateDC(DIB %dx%d at %04x:%04x) = %04x", w, ah, FAR_SEL(info_far), FAR_OFF(info_far), hdc);
    return hdc;
}

/* Debugging (COK_SHOT): save the largest DIB device's pixels next to the screenshot. */

void gdi_debug_dump_dib(const char *path)
{
    DC *dc = g_last_big_dib;
    if (!dc || !path)
        return;
    int n;
    (void)n;
    SDL_Surface *sf = SDL_CreateSurface(dc->dib.w, dc->dib.h, SDL_PIXELFORMAT_XRGB8888);
    if (!sf)
        return;
    dib_refresh_maps(dc);
    for (int y = 0; y < dc->dib.h; y++) {
        const uint8_t *row = surf_row(&dc->dib, y);
        uint32_t *out = (uint32_t *)((uint8_t *)sf->pixels + y * sf->pitch);
        for (int x = 0; x < dc->dib.w; x++) {
            uint8_t v = dc->dib.map ? dc->dib.map[row[x]] : row[x];
            out[x] = (uint32_t)g_syspal[v][0] << 16 | (uint32_t)g_syspal[v][1] << 8 | g_syspal[v][2];
        }
    }
    SDL_SaveBMP(sf, path);
    SDL_DestroySurface(sf);
    const uint8_t *info = (const uint8_t *)guest_ptr(dc->dib_info);
    const uint8_t *t = info + rd32_host(info);
    char line[400];
    int p = 0;
    for (int i = 0; i < 8; i++)
        p += snprintf(line + p, sizeof(line) - p, " %d=%02x%02x%02x", i, t[4 * i + 2], t[4 * i + 1], t[4 * i]);
    for (int i = 248; i < 256; i++)
        p += snprintf(line + p, sizeof(line) - p, " %d=%02x%02x%02x", i, t[4 * i + 2], t[4 * i + 1], t[4 * i]);
    LOGD("DIB colour table:%s", line);
    p = 0;
    for (int i = 0; i < 8; i++)
        p += snprintf(line + p, sizeof(line) - p, " %d=%02x%02x%02x", i, g_syspal[i][0], g_syspal[i][1], g_syspal[i][2]);
    for (int i = 248; i < 256; i++)
        p += snprintf(line + p, sizeof(line) - p, " %d=%02x%02x%02x", i, g_syspal[i][0], g_syspal[i][1], g_syspal[i][2]);
    LOGD("system palette:%s", line);
}

/* HDC CreateDC(LPCSTR driver, LPCSTR device, LPCSTR output, const void FAR *init) */
static void CreateDC(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llll", a);
    const char *drv = guest_ptr(a[0]);
    c->a.x = 0;
    if (drv && !strncmp(drv, "DISPLAY", 7)) {
        DC *dc = dc_new(DC_SCREEN);
        c->a.x = h_alloc(HT_DC, dc);
    } else if (drv && (!SDL_strncasecmp(drv, "DIRDIB", 6) || !SDL_strncasecmp(drv, "DIB", 3)) && a[3]) {
        c->a.x = create_dib_dc(a[3]);
    } else {
        LOGW("CreateDC(%s): only the display and DIB devices are available", drv ? drv : "(null)");
    }
    api_ret(c, n);
}

static void CreateIC(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llll", a);
    const char *drv = guest_ptr(a[0]);
    c->a.x = 0;
    if (drv && !strncmp(drv, "DISPLAY", 7))
        c->a.x = h_alloc(HT_DC, dc_new(DC_INFO));
    api_ret(c, n);
}

static void CreateCompatibleDC(CPU *c) /* HDC CreateCompatibleDC(HDC) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_new(DC_MEMORY);
    dc->hbitmap = g_stock_bitmap;
    DC *src = dc_get((uint16_t)a[0]);
    if (src)
        dc->hpalette = src->hpalette;
    c->a.x = h_alloc(HT_DC, dc);
    api_ret(c, n);
}

static void DeleteDC(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    if (dc && !dc->owned) {
        dc_free((uint16_t)a[0]);
        c->a.x = 1;
    }
    api_ret(c, n);
}

static void GetDeviceCaps(CPU *c)      /* int GetDeviceCaps(HDC, int) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int16_t v = 0;
    switch (a[1]) {
    case 0: v = 0x030A; break;                   /* DRIVERVERSION */
    case 2: v = 1; break;                        /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: v = 208; break;                      /* HORZSIZE (mm) */
    case 6: v = 156; break;                      /* VERTSIZE */
    case 8: v = dc && dc->kind == DC_DIB ? (int16_t)dc->dib.w : SCREEN_W; break;    /* HORZRES */
    case 10: v = dc && dc->kind == DC_DIB ? (int16_t)dc->dib.h : SCREEN_H; break;   /* VERTRES */
    case 12: v = dc && dc->kind == DC_DIB ? (int16_t)dc->dib_bpp : 8; break;    /* BITSPIXEL */
    case 14: v = 1; break;                       /* PLANES */
    case 16: v = -1; break;                      /* NUMBRUSHES */
    case 18: v = 100; break;                     /* NUMPENS */
    case 22: v = 0; break;                       /* NUMFONTS */
    case 24: v = dc && dc->kind == DC_DIB && dc->dib_bpp == 1 ? 2 : 20; break;  /* NUMCOLORS: static colours */
    case 26: v = 0x23; break;                    /* PDEVICESIZE */
    case 28: v = 0; break;                       /* CURVECAPS */
    case 30: v = 0x22; break;                    /* LINECAPS */
    case 32: v = 0x08; break;                    /* POLYGONALCAPS */
    case 34: v = 0x2204; break;                  /* TEXTCAPS */
    case 36: v = 1; break;                       /* CLIPCAPS */
    case 38: v = 0x0001 | 0x0008 | 0x0080 | 0x0100 | 0x0200 | 0x0400 | 0x0800 | 0x2000; break;   /* RASTERCAPS */
    case 40: case 42: v = 36; break;             /* ASPECTX / ASPECTY */
    case 44: v = 51; break;                      /* ASPECTXY */
    case 88: case 90: v = 96; break;             /* LOGPIXELSX / Y */
    case 104: v = 256; break;                    /* SIZEPALETTE */
    case 106: v = 20; break;                     /* NUMRESERVED */
    case 108: v = 18; break;                     /* COLORRES */
    default: v = 0; break;
    }
    c->a.x = (uint16_t)v;
    api_ret(c, n);
}

static void GetDCOrg(CPU *c)           /* DWORD GetDCOrg(HDC) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_get((uint16_t)a[0]);
    IRect o = dc ? dc_origin(dc) : irect(0, 0, 0, 0);
    api_ret32(c, n, (uint32_t)(uint16_t)o.l | (uint32_t)(uint16_t)o.t << 16);
}

static void SaveDC(CPU *c)             /* int SaveDC(HDC) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    if (dc) {
        DC *s = malloc(sizeof(DC));
        *s = *dc;
        rgn_init(&s->clip);
        rgn_copy(&s->clip, &dc->clip);
        rgn_init(&s->paint_rgn);
        rgn_init(&s->cache_clip);
        s->saved = dc->saved;
        dc->saved = s;
        c->a.x = (uint16_t)++dc->nsaved;
    }
    api_ret(c, n);
}

static void RestoreDC(CPU *c)          /* BOOL RestoreDC(HDC, int level): -1 = latest */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int level = (int16_t)a[1];
    c->a.x = 0;
    if (dc && dc->nsaved) {
        if (level < 0)
            level = dc->nsaved + level + 1;
        while (dc->saved && dc->nsaved >= level) {
            DC *s = dc->saved;
            if (dc->nsaved == level) {
                Region paint = dc->paint_rgn, cache = dc->cache_clip;
                int paint_on = dc->paint, owned = dc->owned;
                rgn_free(&dc->clip);
                uint16_t bm_old = dc->hbitmap;
                *dc = *s;
                dc->paint_rgn = paint;
                dc->cache_clip = cache;
                dc->paint = paint_on;
                dc->owned = owned;
                dc->saved = s->saved;
                if (bm_old != dc->hbitmap) {
                    Bitmap *b = h_get(bm_old, HT_BITMAP);
                    if (b) b->dc = 0;
                    b = h_get(dc->hbitmap, HT_BITMAP);
                    if (b) b->dc = (uint16_t)a[0];
                }
                free(s);
            } else {
                dc->saved = s->saved;
                rgn_free(&s->clip);
                free(s);
            }
            dc->nsaved--;
        }
        dc_invalidate(dc);
        c->a.x = 1;
    }
    api_ret(c, n);
}

/* ---- API: attributes ----------------------------------------------------------------------- */

#define DC_ATTR32(fname, field)                                                                 \
    static void Set##fname(CPU *c)                                                              \
    {                                                                                           \
        uint32_t a[2];                                                                          \
        uint16_t n = api_args(c, "wl", a);                                                      \
        DC *dc = dc_get((uint16_t)a[0]);                                                        \
        uint32_t old = dc ? dc->field : 0x80000000u;                                            \
        if (dc) dc->field = a[1];                                                               \
        api_ret32(c, n, old);                                                                   \
    }                                                                                           \
    static void Get##fname(CPU *c)                                                              \
    {                                                                                           \
        uint32_t a[1];                                                                          \
        uint16_t n = api_args(c, "w", a);                                                       \
        DC *dc = dc_get((uint16_t)a[0]);                                                        \
        api_ret32(c, n, dc ? dc->field : 0);                                                    \
    }

#define DC_ATTR16(fname, field)                                                                 \
    static void Set##fname(CPU *c)                                                              \
    {                                                                                           \
        uint32_t a[2];                                                                          \
        uint16_t n = api_args(c, "ww", a);                                                      \
        DC *dc = dc_get((uint16_t)a[0]);                                                        \
        c->a.x = dc ? (uint16_t)dc->field : 0;                                                  \
        if (dc) dc->field = (int)a[1];                                                          \
        api_ret(c, n);                                                                          \
    }                                                                                           \
    static void Get##fname(CPU *c)                                                              \
    {                                                                                           \
        uint32_t a[1];                                                                          \
        uint16_t n = api_args(c, "w", a);                                                       \
        DC *dc = dc_get((uint16_t)a[0]);                                                        \
        c->a.x = dc ? (uint16_t)dc->field : 0;                                                  \
        api_ret(c, n);                                                                          \
    }

DC_ATTR32(TextColor, text_color)
DC_ATTR32(BkColor, bk_color)
DC_ATTR16(BkMode, bk_mode)
DC_ATTR16(ROP2, rop2)
DC_ATTR16(TextAlign, text_align)
DC_ATTR16(StretchBltMode, stretch_mode)
DC_ATTR16(PolyFillMode, poly_fill_mode)
DC_ATTR16(MapMode, map_mode)
DC_ATTR16(TextCharacterExtra, char_extra)

#define DC_POINT(fname, fx, fy, offset)                                                         \
    static void fname(CPU *c)                                                                   \
    {                                                                                           \
        uint32_t a[3];                                                                          \
        uint16_t n = api_args(c, "www", a);                                                     \
        DC *dc = dc_get((uint16_t)a[0]);                                                        \
        uint32_t old = 0;                                                                       \
        if (dc) {                                                                               \
            old = (uint32_t)(uint16_t)dc->fx | (uint32_t)(uint16_t)dc->fy << 16;               \
            dc->fx = (offset ? dc->fx : 0) + (int16_t)a[1];                                     \
            dc->fy = (offset ? dc->fy : 0) + (int16_t)a[2];                                     \
        }                                                                                       \
        api_ret32(c, n, old);                                                                   \
    }

DC_POINT(SetViewportOrg, vp_x, vp_y, 0)
DC_POINT(SetWindowOrg, win_x, win_y, 0)
DC_POINT(OffsetViewportOrg, vp_x, vp_y, 1)
DC_POINT(OffsetWindowOrg, win_x, win_y, 1)
DC_POINT(SetBrushOrg, brush_x, brush_y, 0)
DC_POINT(MoveTo, cur_x, cur_y, 0)

#define DC_GETPOINT(fname, fx, fy)                                                              \
    static void fname(CPU *c)                                                                   \
    {                                                                                           \
        uint32_t a[1];                                                                          \
        uint16_t n = api_args(c, "w", a);                                                       \
        DC *dc = dc_get((uint16_t)a[0]);                                                        \
        api_ret32(c, n, dc ? (uint32_t)(uint16_t)dc->fx | (uint32_t)(uint16_t)dc->fy << 16 : 0); \
    }

DC_GETPOINT(GetViewportOrg, vp_x, vp_y)
DC_GETPOINT(GetWindowOrg, win_x, win_y)
DC_GETPOINT(GetBrushOrg, brush_x, brush_y)
DC_GETPOINT(GetCurrentPosition, cur_x, cur_y)

static void GetViewportExt(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret32(c, n, 1 | 1u << 16);
}

static void SetTextJustification(CPU *c)   /* int SetTextJustification(HDC, int extra, int breaks) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc) {
        dc->break_extra = (int16_t)a[1];
        dc->break_count = (int16_t)a[2];
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void map_points(CPU *c, int to_device)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc) {
        int dx = dc->vp_x - dc->win_x, dy = dc->vp_y - dc->win_y;
        if (!to_device) { dx = -dx; dy = -dy; }
        uint16_t s = FAR_SEL(a[1]), o = FAR_OFF(a[1]);
        for (uint32_t i = 0; i < a[2]; i++, o += 4) {
            wr16(s, o, (uint16_t)((int16_t)rd16(s, o) + dx));
            wr16(s, o + 2, (uint16_t)((int16_t)rd16(s, o + 2) + dy));
        }
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void LPtoDP(CPU *c) { map_points(c, 1); }
static void DPtoLP(CPU *c) { map_points(c, 0); }

/* ---- API: objects --------------------------------------------------------------------------- */

static void GetStockObject(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = gdi_stock((int)a[0]);
    api_ret(c, n);
}

static void select_clip(DC *dc, Region *g)
{
    if (g) {
        rgn_copy(&dc->clip, g);
        dc->has_clip = 1;
    } else {
        dc->has_clip = 0;
        rgn_set_rect(&dc->clip, irect(0, 0, 0, 0));
    }
    dc_invalidate(dc);
}

static void SelectObject(CPU *c)       /* HGDIOBJ SelectObject(HDC, HGDIOBJ) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    uint16_t h = (uint16_t)a[1], old = 0;
    if (dc) {
        switch (h_type(h)) {
        case HT_PEN: old = dc->hpen; dc->hpen = h; break;
        case HT_BRUSH: old = dc->hbrush; dc->hbrush = h; break;
        case HT_FONT: old = dc->hfont; dc->hfont = h; break;
        case HT_BITMAP: {
            Bitmap *b = h_get(h, HT_BITMAP);
            if (dc->kind != DC_MEMORY || (b->dc && b->dc != (uint16_t)a[0] && h != g_stock_bitmap))
                break;
            Bitmap *ob = h_get(dc->hbitmap, HT_BITMAP);
            if (ob && ob->dc == (uint16_t)a[0])
                ob->dc = 0;
            old = dc->hbitmap;
            dc->hbitmap = h;
            if (h != g_stock_bitmap)
                b->dc = (uint16_t)a[0];
            dc_invalidate(dc);
            break;
        }
        case HT_RGN:
            select_clip(dc, h_get(h, HT_RGN));
            old = (uint16_t)rgn_kind(&dc->clip);
            break;
        default:
            break;
        }
    }
    c->a.x = old;
    api_ret(c, n);
}

static void DeleteObject(CPU *c)       /* BOOL DeleteObject(HGDIOBJ) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t h = (uint16_t)a[0];
    c->a.x = 1;
    if (!is_stock(h)) {
        switch (h_type(h)) {
        case HT_PEN: case HT_BRUSH: {
            Brush *b = h_get(h, HT_BRUSH);
            if (b)
                free(b->pat.bits);
            free(h_get(h, h_type(h)));
            h_free(h);
            break;
        }
        case HT_FONT:
            gdi_free_font(h);
            break;
        case HT_BITMAP: {
            Bitmap *b = h_get(h, HT_BITMAP);
            if (b->dc && dc_get(b->dc)) {
                c->a.x = 0;                       /* still selected into a DC */
                break;
            }
            free(b->s.bits);
            free(b);
            h_free(h);
            break;
        }
        case HT_RGN: {
            Region *g = h_get(h, HT_RGN);
            rgn_free(g);
            free(g);
            h_free(h);
            break;
        }
        case HT_PALETTE:
            free(h_get(h, HT_PALETTE));
            h_free(h);
            break;
        default:
            c->a.x = 0;
        }
    }
    api_ret(c, n);
}

static void GetObject(CPU *c)          /* int GetObject(HGDIOBJ, int size, void FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    uint16_t h = (uint16_t)a[0];
    uint8_t buf[64];
    int len = 0;
    memset(buf, 0, sizeof(buf));
    switch (h_type(h)) {
    case HT_PEN: {
        Pen *p = h_get(h, HT_PEN);
        wr16_host(buf, (uint16_t)p->style);
        wr16_host(buf + 2, (uint16_t)p->width);
        wr32_host(buf + 6, p->color);
        len = 10;
        break;
    }
    case HT_BRUSH: {
        Brush *b = h_get(h, HT_BRUSH);
        wr16_host(buf, (uint16_t)b->style);
        wr32_host(buf + 2, b->color);
        wr16_host(buf + 6, (uint16_t)b->hatch);
        len = 8;
        break;
    }
    case HT_BITMAP: {
        Bitmap *b = h_get(h, HT_BITMAP);
        int bpp = b->s.mono ? 1 : 8;
        wr16_host(buf + 2, (uint16_t)b->s.w);
        wr16_host(buf + 4, (uint16_t)b->s.h);
        wr16_host(buf + 6, (uint16_t)(((b->s.w * bpp + 15) / 16) * 2));
        buf[8] = 1;
        buf[9] = (uint8_t)bpp;
        len = 14;
        break;
    }
    case HT_FONT:
        len = gdi_font_logfont(h, buf);
        break;
    case HT_PALETTE: {
        Palette *p = h_get(h, HT_PALETTE);
        wr16_host(buf, (uint16_t)p->n);
        len = 2;
        break;
    }
    }
    if (len > (int)a[1])
        len = (int)a[1];
    if (a[2] && len > 0)
        memcpy(guest_ptr(a[2]), buf, (size_t)len);
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

static void CreatePen(CPU *c)          /* HPEN CreatePen(int style, int width, COLORREF) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    c->a.x = new_pen((int16_t)a[0], (int16_t)a[1], a[2]);
    api_ret(c, n);
}

static void CreatePenIndirect(CPU *c)  /* HPEN CreatePenIndirect(LOGPEN FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    c->a.x = new_pen((int16_t)rd16(s, o), (int16_t)rd16(s, o + 2), rd32(s, o + 6));
    api_ret(c, n);
}

static void CreateSolidBrush(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = new_brush(BS_SOLID, a[0], 0);
    api_ret(c, n);
}

static void CreateHatchBrush(CPU *c)   /* HBRUSH CreateHatchBrush(int, COLORREF) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    c->a.x = new_brush(BS_HATCHED, a[1], (int16_t)a[0]);
    api_ret(c, n);
}

static uint16_t pattern_brush_from(const Surface *src)
{
    uint16_t h = new_brush(BS_PATTERN, 0, 0);
    Brush *b = h_get(h, HT_BRUSH);
    surface_init(&b->pat, 8, 8, src->mono);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            b->pat.bits[y * 8 + x] = src->bits[(y % src->h) * src->stride + (x % src->w)];
    return h;
}

static void CreatePatternBrush(CPU *c) /* HBRUSH CreatePatternBrush(HBITMAP) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Bitmap *bm = h_get((uint16_t)a[0], HT_BITMAP);
    c->a.x = bm ? pattern_brush_from(&bm->s) : 0;
    api_ret(c, n);
}

/* HBRUSH CreateDIBPatternBrush(HGLOBAL packed DIB, UINT usage) */
static void CreateDIBPatternBrush(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Surface s;
    c->a.x = 0;
    uint32_t info = (uint32_t)global_sel((uint16_t)a[0]) << 16;
    if (dib_decode(NULL, info, 0, (int)a[1], 0, -1, &s)) {
        c->a.x = pattern_brush_from(&s);
        free(s.bits);
    }
    api_ret(c, n);
}

static void CreateBrushIndirect(CPU *c)    /* HBRUSH CreateBrushIndirect(LOGBRUSH FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    int style = (int16_t)rd16(s, o);
    uint32_t color = rd32(s, o + 2);
    uint16_t hatch = rd16(s, o + 6);
    if (style == BS_PATTERN) {
        Bitmap *bm = h_get(hatch, HT_BITMAP);
        c->a.x = bm ? pattern_brush_from(&bm->s) : 0;
    } else {
        c->a.x = new_brush(style, color, (int16_t)hatch);
    }
    api_ret(c, n);
}

/* ---- API: regions ---------------------------------------------------------------------------- */

static uint16_t new_rgn(void)
{
    Region *g = calloc(1, sizeof(Region));
    rgn_init(g);
    return h_alloc(HT_RGN, g);
}

static void CreateRectRgn(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwww", a);
    uint16_t h = new_rgn();
    rgn_set_rect(h_get(h, HT_RGN), irect((int16_t)a[0], (int16_t)a[1], (int16_t)a[2], (int16_t)a[3]));
    c->a.x = h;
    api_ret(c, n);
}

static void CreateRectRgnIndirect(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t h = new_rgn();
    rgn_set_rect(h_get(h, HT_RGN), rd_rect(a[0]));
    c->a.x = h;
    api_ret(c, n);
}

static void CreateEllipticRgnIndirect(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t h = new_rgn();
    rgn_ellipse(h_get(h, HT_RGN), rd_rect(a[0]));
    c->a.x = h;
    api_ret(c, n);
}

static void CreateEllipticRgn(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwww", a);
    uint16_t h = new_rgn();
    rgn_ellipse(h_get(h, HT_RGN), irect((int16_t)a[0], (int16_t)a[1], (int16_t)a[2], (int16_t)a[3]));
    c->a.x = h;
    api_ret(c, n);
}

static void CreateRoundRectRgn(CPU *c) /* (l, t, r, b, ew, eh) */
{
    uint32_t a[6];
    uint16_t n = api_args(c, "wwwwww", a);
    uint16_t h = new_rgn();
    rgn_roundrect(h_get(h, HT_RGN), irect((int16_t)a[0], (int16_t)a[1], (int16_t)a[2], (int16_t)a[3]),
                  (int16_t)a[4], (int16_t)a[5]);
    c->a.x = h;
    api_ret(c, n);
}

static int *read_points(uint32_t far, int count)
{
    int *xy = malloc(sizeof(int) * 2 * (size_t)(count > 0 ? count : 1));
    uint16_t s = FAR_SEL(far), o = FAR_OFF(far);
    for (int i = 0; i < count; i++) {
        xy[2 * i] = (int16_t)rd16(s, (uint16_t)(o + 4 * i));
        xy[2 * i + 1] = (int16_t)rd16(s, (uint16_t)(o + 4 * i + 2));
    }
    return xy;
}

static void CreatePolygonRgn(CPU *c)   /* HRGN CreatePolygonRgn(const POINT FAR *, int, int mode) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lww", a);
    int count = (int16_t)a[1];
    int *xy = read_points(a[0], count);
    uint16_t h = new_rgn();
    rgn_polygon(h_get(h, HT_RGN), xy, &count, 1, (int16_t)a[2]);
    free(xy);
    c->a.x = h;
    api_ret(c, n);
}

static void CreatePolyPolygonRgn(CPU *c)   /* (const POINT FAR *, const int FAR *counts, int n, int mode) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llww", a);
    int np = (int16_t)a[2], total = 0;
    int *counts = malloc(sizeof(int) * (size_t)(np > 0 ? np : 1));
    for (int i = 0; i < np; i++) {
        counts[i] = (int16_t)rd16(FAR_SEL(a[1]), (uint16_t)(FAR_OFF(a[1]) + 2 * i));
        total += counts[i];
    }
    int *xy = read_points(a[0], total);
    uint16_t h = new_rgn();
    rgn_polygon(h_get(h, HT_RGN), xy, counts, np, (int16_t)a[3]);
    free(xy);
    free(counts);
    c->a.x = h;
    api_ret(c, n);
}

static void SetRectRgn(CPU *c)         /* void SetRectRgn(HRGN, int, int, int, int) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwww", a);
    Region *g = h_get((uint16_t)a[0], HT_RGN);
    if (g)
        rgn_set_rect(g, irect((int16_t)a[1], (int16_t)a[2], (int16_t)a[3], (int16_t)a[4]));
    api_ret(c, n);
}

static void CombineRgn(CPU *c)         /* int CombineRgn(HRGN dst, HRGN a, HRGN b, int mode) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwww", a);
    Region *d = h_get((uint16_t)a[0], HT_RGN), *x = h_get((uint16_t)a[1], HT_RGN), *y = h_get((uint16_t)a[2], HT_RGN);
    c->a.x = 0;                                    /* ERROR */
    if (d && x && (y || a[3] == RGN_COPY)) {
        rgn_combine(d, x, y ? y : x, (int)a[3]);
        c->a.x = (uint16_t)rgn_kind(d);
    }
    api_ret(c, n);
}

static void OffsetRgn(CPU *c)          /* int OffsetRgn(HRGN, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Region *g = h_get((uint16_t)a[0], HT_RGN);
    c->a.x = 0;
    if (g) {
        rgn_offset(g, (int16_t)a[1], (int16_t)a[2]);
        c->a.x = (uint16_t)rgn_kind(g);
    }
    api_ret(c, n);
}

static void EqualRgn(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Region *x = h_get((uint16_t)a[0], HT_RGN), *y = h_get((uint16_t)a[1], HT_RGN);
    c->a.x = x && y && rgn_equal(x, y);
    api_ret(c, n);
}

static void GetRgnBox(CPU *c)          /* int GetRgnBox(HRGN, RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Region *g = h_get((uint16_t)a[0], HT_RGN);
    c->a.x = 0;
    if (g) {
        wr_rect(a[1], g->n ? g->box : irect(0, 0, 0, 0));
        c->a.x = (uint16_t)rgn_kind(g);
    }
    api_ret(c, n);
}

static void PtInRegion(CPU *c)         /* BOOL PtInRegion(HRGN, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Region *g = h_get((uint16_t)a[0], HT_RGN);
    c->a.x = g && rgn_pt_in(g, (int16_t)a[1], (int16_t)a[2]);
    api_ret(c, n);
}

static void RectInRegion(CPU *c)       /* BOOL RectInRegion(HRGN, const RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Region *g = h_get((uint16_t)a[0], HT_RGN);
    c->a.x = g && rgn_rect_in(g, rd_rect(a[1]));
    api_ret(c, n);
}

/* ---- API: clipping ---------------------------------------------------------------------- */

static void SelectClipRgn(CPU *c)      /* int SelectClipRgn(HDC, HRGN) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    if (dc) {
        select_clip(dc, h_get((uint16_t)a[1], HT_RGN));
        c->a.x = dc->has_clip ? (uint16_t)rgn_kind(&dc->clip) : SIMPLEREGION;
    }
    api_ret(c, n);
}

static IRect logical_to_device(DC *dc, IRect r)
{
    rect_offset(&r, dc->vp_x - dc->win_x, dc->vp_y - dc->win_y);
    return r;
}

static void clip_rect_op(CPU *c, int op)       /* (HDC, l, t, r, b) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    if (dc) {
        IRect r = logical_to_device(dc, irect((int16_t)a[1], (int16_t)a[2], (int16_t)a[3], (int16_t)a[4]));
        if (!dc->has_clip) {
            rgn_set_rect(&dc->clip, dc_extent(dc));
            dc->has_clip = 1;
        }
        Region tmp;
        rgn_init(&tmp);
        rgn_set_rect(&tmp, r);
        rgn_combine(&dc->clip, &dc->clip, &tmp, op);
        rgn_free(&tmp);
        dc_invalidate(dc);
        c->a.x = (uint16_t)rgn_kind(&dc->clip);
    }
    api_ret(c, n);
}

static void IntersectClipRect(CPU *c) { clip_rect_op(c, RGN_AND); }
static void ExcludeClipRect(CPU *c) { clip_rect_op(c, RGN_DIFF); }

static void OffsetClipRgn(CPU *c)      /* int OffsetClipRgn(HDC, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    c->a.x = 0;
    if (dc) {
        if (dc->has_clip)
            rgn_offset(&dc->clip, (int16_t)a[1], (int16_t)a[2]);
        dc_invalidate(dc);
        c->a.x = dc->has_clip ? (uint16_t)rgn_kind(&dc->clip) : SIMPLEREGION;
    }
    api_ret(c, n);
}

static void GetClipBox(CPU *c)         /* int GetClipBox(HDC, RECT FAR *) — logical coordinates */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    IRect r = irect(0, 0, 0, 0);
    int kind = NULLREGION;
    Target t;
    if (dc) {
        if (dc_target(dc, &t) || dc->kind == DC_INFO) {
            r = dc->kind == DC_INFO ? irect(0, 0, SCREEN_W, SCREEN_H) : t.clip->box;
            if (dc->kind != DC_INFO)
                rect_offset(&r, -t.ox, -t.oy);
            kind = dc->kind == DC_INFO ? SIMPLEREGION : rgn_kind(t.clip);
        }
    }
    wr_rect(a[1], r);
    LOGD("GetClipBox(%04x) = %d,%d,%d,%d kind %d", a[0], r.l, r.t, r.r, r.b, kind);
    c->a.x = (uint16_t)kind;
    api_ret(c, n);
}

static void PtVisible(CPU *c)          /* BOOL PtVisible(HDC, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    c->a.x = dc && dc_target(dc, &t) && rgn_pt_in(t.clip, (int16_t)a[1] + t.ox, (int16_t)a[2] + t.oy);
    api_ret(c, n);
}

static void RectVisible(CPU *c)        /* BOOL RectVisible(HDC, const RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Target t;
    c->a.x = 0;
    if (dc && dc_target(dc, &t)) {
        IRect r = rd_rect(a[1]);
        rect_offset(&r, t.ox, t.oy);
        c->a.x = (uint16_t)rgn_rect_in(t.clip, r);
    }
    api_ret(c, n);
}

/* The undocumented visible-region calls USER uses (and Director borrows). Here the
 * visible region is the DC's clip as an app region in device coordinates. */
static void InquireVisRgn(CPU *c)      /* HRGN InquireVisRgn(HDC) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_get((uint16_t)a[0]);
    static uint16_t vis;                           /* GDI owns it; one shared region */
    if (!vis)
        vis = new_rgn();
    Region *g = h_get(vis, HT_RGN);
    Target t;
    if (dc && dc_target(dc, &t)) {
        rgn_copy(g, t.clip);
        IRect o = dc_origin(dc);
        rgn_offset(g, -o.l, -o.t);
    } else {
        rgn_set_rect(g, irect(0, 0, 0, 0));
    }
    c->a.x = vis;
    api_ret(c, n);
}

static void SelectVisRgn(CPU *c)       /* int SelectVisRgn(HDC, HRGN) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Region *g = h_get((uint16_t)a[1], HT_RGN);
    c->a.x = 0;
    if (dc && g) {
        select_clip(dc, g);
        c->a.x = (uint16_t)rgn_kind(g);
    }
    api_ret(c, n);
}

static void SaveVisRgn(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void RestoreVisRgn(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc)
        select_clip(dc, NULL);
    c->a.x = SIMPLEREGION;
    api_ret(c, n);
}

/* ---- misc ------------------------------------------------------------------------------------ */

static void MulDiv(CPU *c)             /* int MulDiv(int, int, int), rounded */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    int32_t x = (int16_t)a[0], y = (int16_t)a[1], z = (int16_t)a[2];
    int32_t r;
    if (z == 0) {
        r = -32768;
    } else {
        int64_t p = (int64_t)x * y;
        int64_t q = ((p < 0) != (z < 0)) ? (p - z / 2) / z : (p + z / 2) / z;
        r = q > 32767 || q < -32768 ? -32768 : (int32_t)q;
    }
    c->a.x = (uint16_t)r;
    api_ret(c, n);
}

static void Escape(CPU *c)             /* int Escape(HDC, int, int, LPCSTR in, void FAR *out) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwll", a);
    LOGD("Escape(%04x, %d): not supported", a[0], (int16_t)a[1]);
    c->a.x = 0;
    api_ret(c, n);
}

const ApiDef g_gdi_api[] = {
    { "GDI", 1, "SetBkColor", SetBkColor },
    { "GDI", 2, "SetBkMode", SetBkMode },
    { "GDI", 3, "SetMapMode", SetMapMode },
    { "GDI", 4, "SetROP2", SetROP2 },
    { "GDI", 6, "SetPolyFillMode", SetPolyFillMode },
    { "GDI", 7, "SetStretchBltMode", SetStretchBltMode },
    { "GDI", 8, "SetTextCharacterExtra", SetTextCharacterExtra },
    { "GDI", 9, "SetTextColor", SetTextColor },
    { "GDI", 10, "SetTextJustification", SetTextJustification },
    { "GDI", 11, "SetWindowOrg", SetWindowOrg },
    { "GDI", 13, "SetViewportOrg", SetViewportOrg },
    { "GDI", 15, "OffsetWindowOrg", OffsetWindowOrg },
    { "GDI", 17, "OffsetViewportOrg", OffsetViewportOrg },
    { "GDI", 20, "MoveTo", MoveTo },
    { "GDI", 21, "ExcludeClipRect", ExcludeClipRect },
    { "GDI", 22, "IntersectClipRect", IntersectClipRect },
    { "GDI", 30, "SaveDC", SaveDC },
    { "GDI", 32, "OffsetClipRgn", OffsetClipRgn },
    { "GDI", 38, "Escape", Escape },
    { "GDI", 39, "RestoreDC", RestoreDC },
    { "GDI", 44, "SelectClipRgn", SelectClipRgn },
    { "GDI", 45, "SelectObject", SelectObject },
    { "GDI", 47, "CombineRgn", CombineRgn },
    { "GDI", 50, "CreateBrushIndirect", CreateBrushIndirect },
    { "GDI", 52, "CreateCompatibleDC", CreateCompatibleDC },
    { "GDI", 53, "CreateDC", CreateDC },
    { "GDI", 54, "CreateEllipticRgn", CreateEllipticRgn },
    { "GDI", 55, "CreateEllipticRgnIndirect", CreateEllipticRgnIndirect },
    { "GDI", 58, "CreateHatchBrush", CreateHatchBrush },
    { "GDI", 60, "CreatePatternBrush", CreatePatternBrush },
    { "GDI", 61, "CreatePen", CreatePen },
    { "GDI", 62, "CreatePenIndirect", CreatePenIndirect },
    { "GDI", 63, "CreatePolygonRgn", CreatePolygonRgn },
    { "GDI", 64, "CreateRectRgn", CreateRectRgn },
    { "GDI", 65, "CreateRectRgnIndirect", CreateRectRgnIndirect },
    { "GDI", 66, "CreateSolidBrush", CreateSolidBrush },
    { "GDI", 67, "DPtoLP", DPtoLP },
    { "GDI", 68, "DeleteDC", DeleteDC },
    { "GDI", 69, "DeleteObject", DeleteObject },
    { "GDI", 72, "EqualRgn", EqualRgn },
    { "GDI", 75, "GetBkColor", GetBkColor },
    { "GDI", 76, "GetBkMode", GetBkMode },
    { "GDI", 77, "GetClipBox", GetClipBox },
    { "GDI", 78, "GetCurrentPosition", GetCurrentPosition },
    { "GDI", 79, "GetDCOrg", GetDCOrg },
    { "GDI", 80, "GetDeviceCaps", GetDeviceCaps },
    { "GDI", 81, "GetMapMode", GetMapMode },
    { "GDI", 82, "GetObject", GetObject },
    { "GDI", 84, "GetPolyFillMode", GetPolyFillMode },
    { "GDI", 85, "GetROP2", GetROP2 },
    { "GDI", 87, "GetStockObject", GetStockObject },
    { "GDI", 88, "GetStretchBltMode", GetStretchBltMode },
    { "GDI", 89, "GetTextCharacterExtra", GetTextCharacterExtra },
    { "GDI", 90, "GetTextColor", GetTextColor },
    { "GDI", 94, "GetViewportExt", GetViewportExt },
    { "GDI", 95, "GetViewportOrg", GetViewportOrg },
    { "GDI", 97, "GetWindowOrg", GetWindowOrg },
    { "GDI", 99, "LPtoDP", LPtoDP },
    { "GDI", 101, "OffsetRgn", OffsetRgn },
    { "GDI", 103, "PtVisible", PtVisible },
    { "GDI", 104, "RectVisible", RectVisible },
    { "GDI", 105, "SelectVisRgn", SelectVisRgn },
    { "GDI", 128, "MulDiv", MulDiv },
    { "GDI", 129, "SaveVisRgn", SaveVisRgn },
    { "GDI", 130, "RestoreVisRgn", RestoreVisRgn },
    { "GDI", 131, "InquireVisRgn", InquireVisRgn },
    { "GDI", 134, "GetRgnBox", GetRgnBox },
    { "GDI", 148, "SetBrushOrg", SetBrushOrg },
    { "GDI", 149, "GetBrushOrg", GetBrushOrg },
    { "GDI", 153, "CreateIC", CreateIC },
    { "GDI", 161, "PtInRegion", PtInRegion },
    { "GDI", 172, "SetRectRgn", SetRectRgn },
    { "GDI", 181, "RectInRegion", RectInRegion },
    { "GDI", 345, "GetTextAlign", GetTextAlign },
    { "GDI", 346, "SetTextAlign", SetTextAlign },
    { "GDI", 451, "CreatePolyPolygonRgn", CreatePolyPolygonRgn },
    { "GDI", 444, "CreateRoundRectRgn", CreateRoundRectRgn },
    { "GDI", 445, "CreateDIBPatternBrush", CreateDIBPatternBrush },
    { "USER", 66, "GetDC", GetDC },
    { "USER", 67, "GetWindowDC", GetWindowDC },
    { "USER", 68, "ReleaseDC", ReleaseDC },
    { NULL, 0, NULL, NULL }
};
