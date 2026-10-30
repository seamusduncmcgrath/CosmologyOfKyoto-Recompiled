/* GDI fonts and text: Windows 2.x/3.x bitmap fonts (FNT resources inside .FON files),
 * font matching for CreateFont, and TextOut/ExtTextOut.
 *
 * Fonts come from the game (AddFontResource, e.g. CKFONT.FON) and, for the stock and
 * system faces, from the host's Windows bitmap fonts if present (vgasys.fon, sserife.fon,
 * ...). Without any font, text calls still measure and succeed but draw nothing.
 */
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "gdi.h"

typedef struct Glyph {
    int width;
    uint32_t offset;             /* of its column-major bitmap in the FNT data */
    uint8_t *mask;               /* decoded on first use: width x height bytes */
} Glyph;

typedef struct FontFace {
    char name[32];
    char file[64];
    uint8_t *data;
    uint32_t size;
    int height, ascent, int_leading, ext_leading, avg_width, max_width, weight, italic, underline, strike;
    int charset, pitch_family, points, first, last, def, brk, vert_res, horz_res;
    Glyph glyph[256];
} FontFace;

typedef struct Font {
    uint8_t lf[50];
    FontFace *face;
    int scale;                   /* integer scaling of a bitmap font */
    int bold, underline, strike;
} Font;

#define MAX_FACES 64
static FontFace *g_face[MAX_FACES];
static int g_nfaces;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }

static int load_fnt(const uint8_t *d, uint32_t size, const char *file)
{
    if (size < 118 || g_nfaces == MAX_FACES)
        return 0;
    uint16_t version = u16(d);
    if ((version != 0x200 && version != 0x300) || (u16(d + 66) & 1))     /* vector fonts: no */
        return 0;
    FontFace *f = calloc(1, sizeof(FontFace));
    f->data = malloc(size);
    memcpy(f->data, d, size);
    f->size = size;
    snprintf(f->file, sizeof(f->file), "%s", file);
    f->points = u16(d + 68);
    f->vert_res = u16(d + 70);
    f->horz_res = u16(d + 72);
    f->ascent = u16(d + 74);
    f->int_leading = u16(d + 76);
    f->ext_leading = u16(d + 78);
    f->italic = d[80];
    f->underline = d[81];
    f->strike = d[82];
    f->weight = u16(d + 83);
    f->charset = d[85];
    f->height = u16(d + 88);
    f->pitch_family = d[90];
    f->avg_width = u16(d + 91);
    f->max_width = u16(d + 93);
    f->first = d[95];
    f->last = d[96];
    f->def = d[97] + f->first;
    f->brk = d[98] + f->first;
    uint32_t face_off = u32(d + 105);
    if (face_off < size)
        snprintf(f->name, sizeof(f->name), "%s", (const char *)d + face_off);
    int v3 = version == 0x300;
    const uint8_t *tab = d + (v3 ? 148 : 118);
    for (int ch = f->first; ch <= f->last; ch++) {
        const uint8_t *e = tab + (ch - f->first) * (v3 ? 6 : 4);
        if (e + (v3 ? 6 : 4) > d + size)
            break;
        f->glyph[ch].width = u16(e);
        f->glyph[ch].offset = v3 ? u32(e + 2) : u16(e + 2);
    }
    g_face[g_nfaces++] = f;
    LOGD("font \"%s\" %dpx (%d pt, weight %d) from %s", f->name, f->height, f->points, f->weight, file);
    return 1;
}

/* Load every FONT resource of a .FON (an NE file) or a bare .FNT. Returns the count. */
static int load_font_file(const char *host, const char *label)
{
    FILE *fp = fopen(host, "rb");
    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len);
    len = (long)fread(d, 1, (size_t)len, fp);
    fclose(fp);
    int count = 0;
    if (len > 0x40 && d[0] == 'M' && d[1] == 'Z') {
        uint32_t ne = u32(d + 0x3C);
        if (ne + 0x40 < (uint32_t)len && d[ne] == 'N' && d[ne + 1] == 'E') {
            const uint8_t *table = d + ne + u16(d + ne + 0x24);
            uint16_t shift = u16(table);
            const uint8_t *p = table + 2;
            while (p + 8 <= d + len && u16(p)) {
                uint16_t type = u16(p), n = u16(p + 2);
                for (uint16_t i = 0; i < n; i++) {
                    const uint8_t *e = p + 8 + 12 * i;
                    uint32_t off = (uint32_t)u16(e) << shift, sz = (uint32_t)u16(e + 2) << shift;
                    if (type == 0x8008 && off + sz <= (uint32_t)len)
                        count += load_fnt(d + off, sz, label);
                }
                p += 8 + 12 * n;
            }
        }
    } else {
        count = load_fnt(d, (uint32_t)len, label);
    }
    free(d);
    return count;
}

static const uint8_t *glyph_mask(FontFace *f, int ch)
{
    Glyph *g = &f->glyph[ch];
    if (g->mask || !g->width)
        return g->mask;
    int w = g->width, h = f->height, cols = (w + 7) / 8;
    if (g->offset + (uint32_t)cols * h > f->size)
        return NULL;
    g->mask = calloc((size_t)w * h, 1);
    for (int c = 0; c < cols; c++)
        for (int y = 0; y < h; y++) {
            uint8_t b = f->data[g->offset + (uint32_t)c * h + y];
            for (int bit = 0; bit < 8; bit++) {
                int x = c * 8 + bit;
                if (x < w && (b & (0x80 >> bit)))
                    g->mask[y * w + x] = 1;
            }
        }
    return g->mask;
}

static int char_index(FontFace *f, uint8_t ch)
{
    if (ch < f->first || ch > f->last || !f->glyph[ch].width)
        return f->def >= f->first && f->def <= f->last ? f->def : f->first;
    return ch;
}

/* ---- font objects and matching --------------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

static FontFace *match(const uint8_t *lf, int *scale)
{
    int height = (int16_t)u16(lf), weight = (int16_t)u16(lf + 8), italic = lf[10];
    const char *face = (const char *)lf + 18;
    FontFace *best = NULL;
    long best_score = -0x7FFFFFFF;
    for (int i = 0; i < g_nfaces; i++) {
        FontFace *f = g_face[i];
        long score = 0;
        if (face[0] && ieq(face, f->name))
            score += 100000;
        int fh = height < 0 ? f->height - f->int_leading : f->height;
        int want = height < 0 ? -height : height;
        if (want == 0)
            want = 13;                             /* the default request: a small UI font */
        /* like Windows' raster font mapper: the largest size not above the request, and a
         * bigger one only when there is nothing smaller (layouts assume text fits) */
        score -= fh <= want ? (long)(want - fh) * 500 : 20000 + (long)(fh - want) * 500;
        score -= abs(f->weight - (weight ? weight : 400)) / 10;
        if (!!f->italic != !!italic)
            score -= 50;
        if (f->charset == 0)
            score += 10;
        if (score > best_score) {
            best_score = score;
            best = f;
        }
    }
    *scale = 1;
    if (best && height) {
        int fh = height < 0 ? best->height - best->int_leading : best->height;
        int want = height < 0 ? -height : height;
        if (fh > 0 && want >= 2 * fh)
            *scale = want / fh;
    }
    return best;
}

static uint16_t new_font(const uint8_t *lf)
{
    Font *f = calloc(1, sizeof(Font));
    memcpy(f->lf, lf, 50);
    f->face = match(lf, &f->scale);
    int weight = (int16_t)u16(lf + 8);
    f->bold = weight >= 600 && f->face && f->face->weight < 600;
    f->underline = lf[11];
    f->strike = lf[12];
    return h_alloc(HT_FONT, f);
}

static uint16_t make_font(int height, int weight, int pitch, const char *face)
{
    uint8_t lf[50];
    memset(lf, 0, sizeof(lf));
    lf[0] = (uint8_t)height;
    lf[1] = (uint8_t)(height >> 8);
    lf[8] = (uint8_t)weight;
    lf[9] = (uint8_t)(weight >> 8);
    lf[17] = (uint8_t)pitch;
    snprintf((char *)lf + 18, 32, "%s", face);
    return new_font(lf);
}

static void load_host_fonts(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    static const char *const files[] = { "vgasys.fon", "vgafix.fon", "vgaoem.fon", "sserife.fon", "serife.fon",
                                         "coure.fon", "smalle.fon", NULL };
    const char *windir = SDL_getenv("COK_BUNDLED_FONTS") ? NULL : getenv("WINDIR");
    char path[600];
    for (int i = 0; files[i]; i++) {
        if (windir) {
            snprintf(path, sizeof(path), "%s/Fonts/%s", windir, files[i]);
            load_font_file(path, files[i]);
        }
    }
    /* elsewhere (or COK_BUNDLED_FONTS=1): Wine's re-creations, converted by tools/winefonts.py
     * and shipped in a "fonts" folder next to the program (in the app bundle on macOS) */
    const char *base = SDL_GetBasePath();
    if (!g_nfaces && base) {
        snprintf(path, sizeof(path), "%sfonts", base);
        int n = 0;
        char **names = SDL_GlobDirectory(path, "*.FNT", SDL_GLOB_CASEINSENSITIVE, &n);
        for (int i = 0; names && i < n; i++) {
            char file[700];
            snprintf(file, sizeof(file), "%s/%s", path, names[i]);
            load_font_file(file, names[i]);
        }
        SDL_free(names);
    }
    /* The game's installer copies CKFONT.FON to WINDOWS\SYSTEM and lists it in WIN.INI
     * [fonts], so Windows loads it at start-up; Director then finds "CKFont" for subtitles
     * and dialogue (without it they fall back to a large MS Sans Serif). */
    if (vfs_host_path(GUEST_DIR "\\CKFONT.FON", path, sizeof(path)))
        load_font_file(path, "CKFONT.FON");
    if (!g_nfaces)
        LOGW("no system bitmap fonts found: text will not be drawn");
}

uint16_t gdi_stock_font(int index)
{
    load_host_fonts();
    switch (index) {
    case 10: return make_font(12, 400, 0x31, "Terminal");          /* OEM_FIXED_FONT */
    case 11: return make_font(13, 400, 0x31, "Courier");           /* ANSI_FIXED_FONT */
    case 12: return make_font(13, 400, 0x22, "MS Sans Serif");     /* ANSI_VAR_FONT */
    case 16: return make_font(15, 400, 0x31, "Fixedsys");          /* SYSTEM_FIXED_FONT */
    default: return make_font(16, 700, 0x22, "System");            /* SYSTEM_FONT, DEVICE_DEFAULT_FONT */
    }
}

void gdi_free_font(uint16_t h)
{
    free(h_get(h, HT_FONT));
    h_free(h);
}

int gdi_font_logfont(uint16_t h, uint8_t *out)
{
    Font *f = h_get(h, HT_FONT);
    if (!f)
        return 0;
    memcpy(out, f->lf, 50);
    return 50;
}

static Font *dc_font(DC *dc)
{
    Font *f = dc ? h_get(dc->hfont, HT_FONT) : NULL;
    return f ? f : h_get(gdi_stock(13), HT_FONT);
}

/* ---- metrics ------------------------------------------------------------------------------ */

static int char_advance(Font *f, uint8_t ch)
{
    if (!f->face)
        return 8;
    return (f->face->glyph[char_index(f->face, ch)].width + f->bold) * f->scale;
}

static int font_height(Font *f) { return f->face ? f->face->height * f->scale : 16; }
static int font_ascent(Font *f) { return f->face ? f->face->ascent * f->scale : 13; }

static int text_width(DC *dc, Font *f, const uint8_t *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        w += char_advance(f, s[i]) + dc->char_extra;
    return w;
}

static void fill_tm(Font *f, uint8_t *tm)
{
    memset(tm, 0, 31);
    FontFace *ff = f->face;
    int s = f->scale;
    int h = font_height(f), asc = font_ascent(f);
    wr16_host(tm, (uint16_t)h);
    wr16_host(tm + 2, (uint16_t)asc);
    wr16_host(tm + 4, (uint16_t)(h - asc));
    wr16_host(tm + 6, (uint16_t)(ff ? ff->int_leading * s : 3));
    wr16_host(tm + 8, (uint16_t)(ff ? ff->ext_leading * s : 0));
    wr16_host(tm + 10, (uint16_t)(ff ? ff->avg_width * s + f->bold : 7));
    wr16_host(tm + 12, (uint16_t)(ff ? ff->max_width * s + f->bold : 14));
    wr16_host(tm + 14, (uint16_t)(f->bold ? 700 : ff ? ff->weight : 400));
    tm[16] = ff ? (uint8_t)ff->italic : 0;
    tm[17] = (uint8_t)f->underline;
    tm[18] = (uint8_t)f->strike;
    tm[19] = ff ? (uint8_t)ff->first : 32;
    tm[20] = ff ? (uint8_t)ff->last : 255;
    tm[21] = ff ? (uint8_t)ff->def : 128;
    tm[22] = ff ? (uint8_t)ff->brk : 32;
    tm[23] = ff ? (uint8_t)ff->pitch_family : 0x22;
    tm[24] = ff ? (uint8_t)ff->charset : 0;
    wr16_host(tm + 25, (uint16_t)f->bold);
    wr16_host(tm + 27, (uint16_t)(ff ? ff->horz_res : 96));
    wr16_host(tm + 29, (uint16_t)(ff ? ff->vert_res : 96));
}

/* ---- drawing ----------------------------------------------------------------------------- */

/* Draw a string at logical position (x, y) of the text's reference point (the target's
 * offset maps logical coordinates to the surface). dx: per-character advances (may be NULL).
 * opaque_rect: ETO_OPAQUE rectangle, clip_rect: ETO_CLIPPED, both logical. */
static int draw_text(DC *dc, int x, int y, const uint8_t *s, int n, const int16_t *dx, const IRect *opaque_rect,
                     const IRect *clip_rect)
{
    Font *f = dc_font(dc);
    int w = 0;
    for (int i = 0; i < n; i++)
        w += dx ? dx[i] : char_advance(f, s[i]) + dc->char_extra;
    if (dc->break_count > 0)
        w += dc->break_extra;
    int h = font_height(f), asc = font_ascent(f);
    int align = dc->text_align;
    if (align & 1) {                              /* TA_UPDATECP */
        x = dc->cur_x;
        y = dc->cur_y;
    }
    int left = (align & 6) == 6 ? x - w / 2 : (align & 6) == 2 ? x - w : x;
    int top = (align & 24) == 24 ? y - asc : (align & 24) == 8 ? y - h : y;
    if (align & 1)
        dc->cur_x += (align & 6) == 2 ? -w : (align & 6) == 6 ? 0 : w;
    Target t;
    if (!dc_target(dc, &t))
        return w;
    Region clip;
    rgn_init(&clip);
    rgn_copy(&clip, t.clip);
    if (clip_rect) {
        IRect cr = *clip_rect;
        rect_offset(&cr, t.ox, t.oy);
        rgn_clip_rect(&clip, cr);
    }
    Target tc = t;
    tc.clip = &clip;
    uint8_t fg = dc_color(dc, dc->text_color), bg = dc_color(dc, dc->bk_color);
    if (opaque_rect) {
        IRect r = *opaque_rect;
        rect_offset(&r, t.ox, t.oy);
        blit_fill(&tc, r, bg);
    }
    int sx = left + t.ox, sy = top + t.oy;
    if (dc->bk_mode == 2 && !opaque_rect)        /* OPAQUE: the text cell */
        blit_fill(&tc, irect(sx, sy, sx + w, sy + h), bg);
    int pen = sx;
    int breaks_left = dc->break_count;
    for (int i = 0; i < n; i++) {
        int adv = dx ? dx[i] : char_advance(f, s[i]) + dc->char_extra;
        if (f->face) {
            int ci = char_index(f->face, s[i]);
            const uint8_t *m = glyph_mask(f->face, ci);
            int gw = f->face->glyph[ci].width, gh = f->face->height;
            if (m) {
                if (f->scale == 1) {
                    blit_mono_mask(&tc, pen, sy, m, gw, gh, gw, fg);
                    if (f->bold)
                        blit_mono_mask(&tc, pen + 1, sy, m, gw, gh, gw, fg);
                } else {
                    int sc = f->scale;
                    uint8_t *big = calloc((size_t)gw * sc * gh * sc, 1);
                    for (int yy = 0; yy < gh * sc; yy++)
                        for (int xx = 0; xx < gw * sc; xx++)
                            big[yy * gw * sc + xx] = m[(yy / sc) * gw + xx / sc];
                    blit_mono_mask(&tc, pen, sy, big, gw * sc, gh * sc, gw * sc, fg);
                    if (f->bold)
                        blit_mono_mask(&tc, pen + 1, sy, big, gw * sc, gh * sc, gw * sc, fg);
                    free(big);
                }
            }
        }
        pen += adv;
        if (breaks_left > 0 && f->face && s[i] == f->face->brk) {
            int extra = dc->break_extra / dc->break_count;
            pen += extra;
            breaks_left--;
        }
    }
    if (f->underline)
        blit_fill(&tc, irect(sx, sy + asc + 1, sx + w, sy + asc + 2), fg);
    if (f->strike)
        blit_fill(&tc, irect(sx, sy + asc - asc / 3, sx + w, sy + asc - asc / 3 + 1), fg);
    rgn_free(&clip);
    return w;
}

/* ---- for USER: captions and controls ---------------------------------------------------- */

int gdi_text_extent(DC *dc, const char *s, int n, int *height)
{
    Font *f = dc_font(dc);
    if (height)
        *height = font_height(f);
    return text_width(dc, f, (const uint8_t *)s, n);
}

void gdi_text_out(DC *dc, int x, int y, const char *s, int n)
{
    int align = dc->text_align;
    dc->text_align = 0;
    draw_text(dc, x, y, (const uint8_t *)s, n, NULL, NULL, NULL);
    dc->text_align = align;
}

/* DrawText-like layout in logical coordinates: word wrap (unless !wrap), '\n' breaks,
 * align 0 left / 1 centre / 2 right, '&' mnemonics underlined unless noprefix. Returns the
 * height used. */
int gdi_draw_text_box(DC *dc, IRect r, const char *text, int align, int wrap, int noprefix, int vcenter)
{
    Font *f = dc_font(dc);
    int lh = font_height(f);
    /* strip prefixes, remembering the underlined character */
    char buf[1024];
    int ul = -1, n = 0;
    for (const char *p = text; *p && n < (int)sizeof(buf) - 1; p++) {
        if (!noprefix && *p == '&') {
            if (p[1] == '&') {
                buf[n++] = '&';
                p++;
            } else if (p[1]) {
                ul = n;
            }
            continue;
        }
        buf[n++] = *p;
    }
    buf[n] = 0;
    /* break into lines */
    struct { int start, len; } lines[64];
    int nl = 0, i = 0;
    while (i <= n && nl < 64) {
        int start = i, last_space = -1, w = 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') {
            int cw = char_advance(f, (uint8_t)buf[i]) + dc->char_extra;
            if (wrap && w + cw > r.r - r.l && i > start) {
                if (last_space > start)
                    i = last_space;
                break;
            }
            if (buf[i] == ' ')
                last_space = i;
            w += cw;
            i++;
        }
        int len = i - start;
        while (len > 0 && buf[start + len - 1] == ' ')
            len--;
        lines[nl].start = start;
        lines[nl].len = len;
        nl++;
        if (i < n && buf[i] == '\r') i++;
        if (i < n && buf[i] == '\n') i++;
        else if (i < n && buf[i] == ' ') i++;
        if (i >= n)
            break;
    }
    int total = nl * lh;
    int y = vcenter ? r.t + (r.b - r.t - total) / 2 : r.t;
    for (int k = 0; k < nl; k++, y += lh) {
        int w = text_width(dc, f, (const uint8_t *)buf + lines[k].start, lines[k].len);
        int x = align == 1 ? r.l + (r.r - r.l - w) / 2 : align == 2 ? r.r - w : r.l;
        int saved = dc->text_align;
        dc->text_align = 0;
        draw_text(dc, x, y, (const uint8_t *)buf + lines[k].start,
                  lines[k].len, NULL, NULL, NULL);
        dc->text_align = saved;
        if (ul >= lines[k].start && ul < lines[k].start + lines[k].len) {
            int ux = x + text_width(dc, f, (const uint8_t *)buf + lines[k].start, ul - lines[k].start);
            int uw = char_advance(f, (uint8_t)buf[ul]);
            Target t;
            if (dc_target(dc, &t)) {
                int dx = ux + t.ox, sy = y + font_ascent(f) + 1 + t.oy;
                blit_fill(&t, irect(dx, sy, dx + uw, sy + 1), dc_color(dc, dc->text_color));
            }
        }
    }
    return total;
}

/* ---- API ---------------------------------------------------------------------------------- */

static void TextOut(CPU *c)            /* BOOL TextOut(HDC, int x, int y, LPCSTR, int n) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    int len = (int16_t)a[4];
    if (dc && len > 0) {
        LOGD("TextOut(%04x kind %d hwnd %04x, %d,%d, \"%.*s\") colours %06x/%06x mode %d rop2 %d", (uint16_t)a[0],
             dc->kind, dc->hwnd, (int16_t)a[1], (int16_t)a[2], len, (const char *)guest_ptr(a[3]), dc->text_color,
             dc->bk_color, dc->bk_mode, dc->rop2);
        draw_text(dc, (int16_t)a[1], (int16_t)a[2],
                  (const uint8_t *)guest_ptr(a[3]), len, NULL, NULL, NULL);
    }
    c->a.x = 1;
    api_ret(c, n);
}

/* BOOL ExtTextOut(HDC, int x, int y, UINT options, const RECT FAR *, LPCSTR, UINT n, int FAR *dx) */
static void ExtTextOut(CPU *c)
{
    uint32_t a[8];
    uint16_t n = api_args(c, "wwwwllwl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    if (dc) {
        int len = (int16_t)a[6];
        IRect r, *opaque = NULL, *clip = NULL;
        if (a[4]) {
            r = rd_rect(a[4]);
            if (a[3] & 2) opaque = &r;
            if (a[3] & 4) clip = &r;
        }
        int16_t *dx = NULL;
        if (a[7] && len > 0) {
            dx = malloc(sizeof(int16_t) * (size_t)len);
            for (int i = 0; i < len; i++)
                dx[i] = (int16_t)rd16(FAR_SEL(a[7]), (uint16_t)(FAR_OFF(a[7]) + 2 * i));
        }
        if (len > 0 || opaque) {
            if (len > 0)
                draw_text(dc, (int16_t)a[1], (int16_t)a[2], (const uint8_t *)guest_ptr(a[5]), len, dx,
                          opaque, clip);
            else {
                Target t;
                if (dc_target(dc, &t)) {
                    IRect o = *opaque;
                    rect_offset(&o, t.ox, t.oy);
                    blit_fill(&t, o, dc_color(dc, dc->bk_color));
                }
            }
        }
        free(dx);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void GetTextExtent(CPU *c)      /* DWORD GetTextExtent(HDC, LPCSTR, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    DC *dc = dc_get((uint16_t)a[0]);
    uint32_t r = 0;
    if (dc) {
        Font *f = dc_font(dc);
        int len = (int16_t)a[2];
        int w = len > 0 ? text_width(dc, f, (const uint8_t *)guest_ptr(a[1]), len) : 0;
        r = (uint32_t)(uint16_t)w | (uint32_t)(uint16_t)font_height(f) << 16;
    }
    api_ret32(c, n, r);
}

static void GetTextMetrics(CPU *c)     /* BOOL GetTextMetrics(HDC, TEXTMETRIC FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    DC *dc = dc_get((uint16_t)a[0]);
    uint8_t tm[31];
    fill_tm(dc_font(dc), tm);
    memcpy(guest_ptr(a[1]), tm, sizeof(tm));
    c->a.x = 1;
    api_ret(c, n);
}

static void GetTextFace(CPU *c)        /* int GetTextFace(HDC, int, LPSTR) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    Font *f = dc_font(dc_get((uint16_t)a[0]));
    const char *name = f->face ? f->face->name : (const char *)f->lf + 18;
    size_t len = strlen(name);
    c->a.x = 0;
    if (a[1]) {
        if (len > a[1] - 1)
            len = a[1] - 1;
        memcpy(guest_ptr(a[2]), name, len);
        guest_ptr(a[2])[len] = 0;
        c->a.x = (uint16_t)len;
    }
    api_ret(c, n);
}

static void GetCharWidth(CPU *c)       /* BOOL GetCharWidth(HDC, UINT first, UINT last, int FAR *) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    Font *f = dc_font(dc_get((uint16_t)a[0]));
    for (uint32_t ch = a[1], i = 0; ch <= a[2] && ch < 256; ch++, i++)
        wr16(FAR_SEL(a[3]), (uint16_t)(FAR_OFF(a[3]) + 2 * i), (uint16_t)char_advance(f, (uint8_t)ch));
    c->a.x = 1;
    api_ret(c, n);
}

/* HFONT CreateFont(int h, int w, int esc, int orient, int weight, BYTE italic, BYTE underline,
 *                  BYTE strike, BYTE charset, BYTE out, BYTE clip, BYTE quality, BYTE pitch, LPCSTR face) */
static void CreateFont(CPU *c)
{
    uint32_t a[14];
    uint16_t n = api_args(c, "wwwwwwwwwwwwwl", a);
    uint8_t lf[50];
    memset(lf, 0, sizeof(lf));
    for (int i = 0; i < 5; i++)
        wr16_host(lf + 2 * i, (uint16_t)a[i]);
    for (int i = 0; i < 8; i++)
        lf[10 + i] = (uint8_t)a[5 + i];
    if (a[13] && FAR_SEL(a[13]))
        snprintf((char *)lf + 18, 32, "%s", guest_ptr(a[13]));
    c->a.x = new_font(lf);
    Font *f = h_get(c->a.x, HT_FONT);
    LOGD("CreateFont(%d, \"%s\") = %04x -> %s %dpx", (int16_t)a[0], (const char *)lf + 18, c->a.x,
         f->face ? f->face->name : "(none)", font_height(f));
    api_ret(c, n);
}

static void CreateFontIndirect(CPU *c) /* HFONT CreateFontIndirect(const LOGFONT FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = new_font((const uint8_t *)guest_ptr(a[0]));
    api_ret(c, n);
}

static void AddFontResource(CPU *c)    /* int AddFontResource(LPCSTR file) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    int count = 0;
    if (FAR_SEL(a[0])) {
        const char *name = guest_ptr(a[0]);
        for (int i = 0; i < g_nfaces; i++)
            if (ieq(g_face[i]->file, name))
                count++;
        char host[600];
        if (!count && vfs_host_path(name, host, sizeof(host)))
            count = load_font_file(host, name);
        LOGI("AddFontResource(%s) = %d", name, count);
    }
    c->a.x = (uint16_t)count;
    api_ret(c, n);
}

static void RemoveFontResource(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = 1;
    api_ret(c, n);
}

/* int EnumFonts(HDC, LPCSTR face, FONTENUMPROC, LPSTR data): proc(LOGFONT FAR *, TEXTMETRIC FAR *,
 * int type, LPSTR data) per font (all faces when face is NULL, else that face's sizes). */
static void EnumFonts(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wlll", a);
    const char *face = a[1] && FAR_SEL(a[1]) ? guest_ptr(a[1]) : NULL;
    uint32_t r = 1;
    for (int i = 0; i < g_nfaces && r; i++) {
        FontFace *f = g_face[i];
        if (face ? !ieq(face, f->name) : 0)
            continue;
        if (!face) {                               /* one call per face name */
            int seen = 0;
            for (int j = 0; j < i && !seen; j++)
                seen = ieq(g_face[j]->name, f->name);
            if (seen)
                continue;
        }
        uint16_t sp = c->sp.x;
        c->sp.x -= 50 + 32;
        uint16_t lf_off = c->sp.x, tm_off = (uint16_t)(c->sp.x + 50);
        uint8_t *lf = mem_ptr(c->ss, lf_off);
        memset(lf, 0, 50);
        wr16_host(lf, (uint16_t)f->height);
        wr16_host(lf + 2, (uint16_t)f->avg_width);
        wr16_host(lf + 8, (uint16_t)f->weight);
        lf[10] = (uint8_t)f->italic;
        lf[14] = (uint8_t)f->charset;
        lf[17] = (uint8_t)f->pitch_family;
        snprintf((char *)lf + 18, 32, "%s", f->name);
        Font tmp = { { 0 }, f, 1, 0, 0, 0 };
        fill_tm(&tmp, mem_ptr(c->ss, tm_off));
        push32(c, (uint32_t)c->ss << 16 | lf_off);
        push32(c, (uint32_t)c->ss << 16 | tm_off);
        push16(c, 1);                              /* RASTER_FONTTYPE */
        push32(c, a[3]);
        uint16_t ds = c->ds;
        c->a.x = c->ds;
        rt_callback(c, FAR_SEL(a[2]), FAR_OFF(a[2]));
        r = c->a.x;
        c->ds = ds;
        c->sp.x = sp;
    }
    c->a.x = (uint16_t)r;
    api_ret(c, n);
}

static void SetMapperFlags(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    api_ret32(c, n, 0);
}

const ApiDef g_gdi_text_api[] = {
    { "GDI", 33, "TextOut", TextOut },
    { "GDI", 56, "CreateFont", CreateFont },
    { "GDI", 57, "CreateFontIndirect", CreateFontIndirect },
    { "GDI", 70, "EnumFonts", EnumFonts },
    { "GDI", 91, "GetTextExtent", GetTextExtent },
    { "GDI", 92, "GetTextFace", GetTextFace },
    { "GDI", 93, "GetTextMetrics", GetTextMetrics },
    { "GDI", 119, "AddFontResource", AddFontResource },
    { "GDI", 136, "RemoveFontResource", RemoveFontResource },
    { "GDI", 349, "SetMapperFlags", SetMapperFlags },
    { "GDI", 350, "GetCharWidth", GetCharWidth },
    { "GDI", 351, "ExtTextOut", ExtTextOut },
    { NULL, 0, NULL, NULL }
};
