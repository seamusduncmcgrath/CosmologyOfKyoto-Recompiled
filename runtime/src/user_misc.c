/* The rest of USER: rectangles, strings and wsprintf, system metrics and colours, cursors
 * and icons, string/bitmap resources, MessageBox, menus (data only: nothing draws them),
 * the clipboard, atoms, carets, scroll bars and dialog stubs.
 */
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "gdi.h"

/* ---- rectangles -------------------------------------------------------------------------- */

static void SetRect(CPU *c)            /* void SetRect(RECT FAR *, int, int, int, int) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "lwwww", a);
    wr_rect(a[0], irect((int16_t)a[1], (int16_t)a[2], (int16_t)a[3], (int16_t)a[4]));
    api_ret(c, n);
}

static void SetRectEmpty(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    wr_rect(a[0], irect(0, 0, 0, 0));
    api_ret(c, n);
}

static void CopyRect(CPU *c)           /* void CopyRect(RECT FAR *dst, const RECT FAR *src) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    wr_rect(a[0], rd_rect(a[1]));
    c->a.x = 1;
    api_ret(c, n);
}

static void IsRectEmpty(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    IRect r = rd_rect(a[0]);
    c->a.x = rect_empty(&r);
    api_ret(c, n);
}

static void PtInRect(CPU *c)           /* BOOL PtInRect(const RECT FAR *, POINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    IRect r = rd_rect(a[0]);
    c->a.x = rect_has_pt(&r, (int16_t)(a[1] & 0xFFFF), (int16_t)(a[1] >> 16));
    api_ret(c, n);
}

static void OffsetRect(CPU *c)         /* void OffsetRect(RECT FAR *, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lww", a);
    IRect r = rd_rect(a[0]);
    rect_offset(&r, (int16_t)a[1], (int16_t)a[2]);
    wr_rect(a[0], r);
    api_ret(c, n);
}

static void InflateRect(CPU *c)        /* void InflateRect(RECT FAR *, int, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lww", a);
    IRect r = rd_rect(a[0]);
    int dx = (int16_t)a[1], dy = (int16_t)a[2];
    wr_rect(a[0], irect(r.l - dx, r.t - dy, r.r + dx, r.b + dy));
    api_ret(c, n);
}

static void IntersectRect(CPU *c)      /* BOOL IntersectRect(RECT FAR *, const RECT FAR *, const RECT FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lll", a);
    IRect x = rd_rect(a[1]), y = rd_rect(a[2]), r;
    c->a.x = (uint16_t)rect_intersect(&r, &x, &y);
    wr_rect(a[0], r);
    api_ret(c, n);
}

static void UnionRect(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lll", a);
    IRect x = rd_rect(a[1]), y = rd_rect(a[2]), r;
    rect_union(&r, &x, &y);
    wr_rect(a[0], r);
    c->a.x = !rect_empty(&r);
    api_ret(c, n);
}

static void SubtractRect(CPU *c)       /* BOOL SubtractRect(RECT FAR *, const RECT FAR *, const RECT FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lll", a);
    IRect r = rd_rect(a[1]), s = rd_rect(a[2]);
    if (s.t <= r.t && s.b >= r.b) {
        if (s.l <= r.l && s.r > r.l) r.l = s.r;
        else if (s.r >= r.r && s.l < r.r) r.r = s.l;
    } else if (s.l <= r.l && s.r >= r.r) {
        if (s.t <= r.t && s.b > r.t) r.t = s.b;
        else if (s.b >= r.b && s.t < r.b) r.b = s.t;
    }
    if (rect_empty(&r))
        r = irect(0, 0, 0, 0);
    wr_rect(a[0], r);
    c->a.x = !rect_empty(&r);
    api_ret(c, n);
}

static void EqualRect(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    IRect x = rd_rect(a[0]), y = rd_rect(a[1]);
    c->a.x = memcmp(&x, &y, sizeof(x)) == 0;
    api_ret(c, n);
}

/* ---- strings ---------------------------------------------------------------------------- */

static int ansi_upper(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) ? ch - 32 : ch; }
static int ansi_lower(int ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch + 32 : ch; }

static void lstrcmp_common(CPU *c, int nocase)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    const uint8_t *x = (const uint8_t *)guest_ptr(a[0]), *y = (const uint8_t *)guest_ptr(a[1]);
    int r = 0;
    for (;; x++, y++) {
        int cx = nocase ? ansi_lower(*x) : *x, cy = nocase ? ansi_lower(*y) : *y;
        if (cx != cy) {
            r = cx < cy ? -1 : 1;
            break;
        }
        if (!cx)
            break;
    }
    c->a.x = (uint16_t)r;
    api_ret(c, n);
}

static void lstrcmp(CPU *c) { lstrcmp_common(c, 0); }
static void lstrcmpi(CPU *c) { lstrcmp_common(c, 1); }

/* AnsiUpper/AnsiLower take a string or (selector 0) a character */
static void ansi_case(CPU *c, int upper)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    if (FAR_SEL(a[0]) == 0) {
        int ch = FAR_OFF(a[0]) & 0xFF;
        api_ret32(c, n, (uint32_t)(upper ? ansi_upper(ch) : ansi_lower(ch)));
        return;
    }
    for (uint8_t *p = (uint8_t *)guest_ptr(a[0]); *p; p++)
        *p = (uint8_t)(upper ? ansi_upper(*p) : ansi_lower(*p));
    api_ret32(c, n, a[0]);
}

static void AnsiUpper(CPU *c) { ansi_case(c, 1); }
static void AnsiLower(CPU *c) { ansi_case(c, 0); }

static void ansi_case_buff(CPU *c, int upper)  /* UINT AnsiUpperBuff(LPSTR, UINT n) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    uint32_t len = a[1] ? a[1] : 65536;
    uint8_t *p = (uint8_t *)guest_ptr(a[0]);
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)(upper ? ansi_upper(p[i]) : ansi_lower(p[i]));
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

static void AnsiUpperBuff(CPU *c) { ansi_case_buff(c, 1); }
static void AnsiLowerBuff(CPU *c) { ansi_case_buff(c, 0); }

static void AnsiNext(CPU *c)           /* LPSTR AnsiNext(LPCSTR) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    api_ret32(c, n, *guest_ptr(a[0]) ? a[0] + 1 : a[0]);
}

static void AnsiPrev(CPU *c)           /* LPSTR AnsiPrev(LPCSTR start, LPCSTR cur) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    api_ret32(c, n, FAR_OFF(a[1]) > FAR_OFF(a[0]) ? a[1] - 1 : a[0]);
}

static void char_class(CPU *c, int kind)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    int ch = a[0] & 0xFF;
    int r = 0;
    switch (kind) {
    case 0: r = isalpha(ch) || ch >= 0xC0; break;
    case 1: r = isalnum(ch) || ch >= 0xC0; break;
    case 2: r = ansi_lower(ch) != ch; break;
    case 3: r = ansi_upper(ch) != ch; break;
    }
    c->a.x = (uint16_t)r;
    api_ret(c, n);
}

static void IsCharAlpha(CPU *c) { char_class(c, 0); }
static void IsCharAlphaNumeric(CPU *c) { char_class(c, 1); }
static void IsCharUpper(CPU *c) { char_class(c, 2); }
static void IsCharLower(CPU *c) { char_class(c, 3); }

/* Windows' wsprintf: %[-#0][width][.prec][l|h]{c,d,i,u,x,X,s}, at most 1024 characters. Arguments
 * are read from guest memory: 2 bytes each, 4 for l-modified and %s (far pointers). */
static int format(char *out, const char *fmt, uint16_t as, uint16_t ao)
{
    char *o = out, *end = out + 1023;
    while (*fmt && o < end) {
        if (*fmt != '%') {
            *o++ = *fmt++;
            continue;
        }
        const char *start = fmt++;
        if (*fmt == '%') {
            *o++ = '%';
            fmt++;
            continue;
        }
        char spec[32];
        int si = 0;
        spec[si++] = '%';
        while (*fmt == '-' || *fmt == '#' || *fmt == '0')
            spec[si++] = *fmt++;
        while (isdigit((unsigned char)*fmt) && si < 20)
            spec[si++] = *fmt++;
        if (*fmt == '.') {
            spec[si++] = *fmt++;
            while (isdigit((unsigned char)*fmt) && si < 26)
                spec[si++] = *fmt++;
        }
        int lng = 0;
        if (*fmt == 'l' || *fmt == 'L') { lng = 1; fmt++; }
        else if (*fmt == 'h') fmt++;
        char type = *fmt ? *fmt++ : 0;
        char tmp[1100];
        tmp[0] = 0;
        switch (type) {
        case 'c':
            spec[si++] = 'c';
            spec[si] = 0;
            snprintf(tmp, sizeof(tmp), spec, rd16(as, ao) & 0xFF);
            ao += 2;
            break;
        case 'd': case 'i':
            spec[si++] = 'l'; spec[si++] = 'd'; spec[si] = 0;
            snprintf(tmp, sizeof(tmp), spec, lng ? (long)(int32_t)rd32(as, ao) : (long)(int16_t)rd16(as, ao));
            ao += lng ? 4 : 2;
            break;
        case 'u': case 'x': case 'X':
            spec[si++] = 'l'; spec[si++] = type; spec[si] = 0;
            snprintf(tmp, sizeof(tmp), spec, lng ? (unsigned long)rd32(as, ao) : (unsigned long)rd16(as, ao));
            ao += lng ? 4 : 2;
            break;
        case 's': {
            uint32_t p = rd32(as, ao);
            ao += 4;
            spec[si++] = 's';
            spec[si] = 0;
            snprintf(tmp, sizeof(tmp), spec, p ? guest_ptr(p) : "(null)");
            break;
        }
        default:
            snprintf(tmp, sizeof(tmp), "%.*s", (int)(fmt - start), start);
            break;
        }
        for (const char *t = tmp; *t && o < end; t++)
            *o++ = *t;
    }
    *o = 0;
    return (int)(o - out);
}

/* int FAR CDECL wsprintf(LPSTR out, LPCSTR fmt, ...): the caller pops the arguments */
static void wsprintf(CPU *c)
{
    uint32_t out = rd32(c->ss, (uint16_t)(c->sp.x + 4));
    uint32_t fmt = rd32(c->ss, (uint16_t)(c->sp.x + 8));
    char buf[1100];
    int len = format(buf, guest_ptr(fmt), c->ss, (uint16_t)(c->sp.x + 12));
    memcpy(guest_ptr(out), buf, (size_t)len + 1);
    c->a.x = (uint16_t)len;
    api_ret(c, 0);
}

static void wvsprintf(CPU *c)          /* int wvsprintf(LPSTR, LPCSTR, const void FAR *args) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lll", a);
    char buf[1100];
    int len = format(buf, guest_ptr(a[1]), FAR_SEL(a[2]), FAR_OFF(a[2]));
    memcpy(guest_ptr(a[0]), buf, (size_t)len + 1);
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

/* ---- system metrics and colours ------------------------------------------------------- */

static void GetSystemMetrics(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    static const int16_t m[] = {
        SCREEN_W, SCREEN_H,  /* 0 SM_CXSCREEN, SM_CYSCREEN */
        17, 17,              /* SM_CXVSCROLL, SM_CYHSCROLL */
        20,                  /* SM_CYCAPTION */
        1, 1,                /* SM_CXBORDER, SM_CYBORDER */
        4, 4,                /* SM_CXDLGFRAME, SM_CYDLGFRAME */
        17, 17,              /* SM_CYVTHUMB, SM_CXHTHUMB */
        32, 32, 32, 32,      /* SM_CXICON, SM_CYICON, SM_CXCURSOR, SM_CYCURSOR */
        18,                  /* SM_CYMENU */
        SCREEN_W, SCREEN_H - 20,     /* SM_CXFULLSCREEN, SM_CYFULLSCREEN */
        0,                   /* SM_CYKANJIWINDOW */
        1,                   /* SM_MOUSEPRESENT */
        17, 17,              /* SM_CYVSCROLL, SM_CXHSCROLL */
        0,                   /* SM_DEBUG */
        0,                   /* SM_SWAPBUTTON */
        0, 0, 0, 0,          /* reserved */
        102, 26,             /* SM_CXMIN, SM_CYMIN */
        18, 18,              /* SM_CXSIZE, SM_CYSIZE */
        4, 4,                /* SM_CXFRAME, SM_CYFRAME */
        102, 26,             /* SM_CXMINTRACK, SM_CYMINTRACK */
        4, 4,                /* SM_CXDOUBLECLK, SM_CYDOUBLECLK */
        75, 75,              /* SM_CXICONSPACING, SM_CYICONSPACING */
        0,                   /* SM_MENUDROPALIGNMENT */
        0,                   /* SM_PENWINDOWS */
        0,                   /* SM_DBCSENABLED */
    };
    c->a.x = a[0] < sizeof(m) / sizeof(m[0]) ? (uint16_t)m[a[0]] : 0;
    api_ret(c, n);
}

static uint32_t g_sys_color[21] = {
    0xC0C0C0, 0xC0C0C0, 0x800000, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x000000, 0x000000, 0x000000, 0xFFFFFF,
    0xC0C0C0, 0xC0C0C0, 0xFFFFFF, 0x800000, 0xFFFFFF, 0xC0C0C0, 0x808080, 0x808080, 0x000000, 0x000000,
    0xFFFFFF,
};

uint32_t user_sys_color(int i) { return i >= 0 && i < 21 ? g_sys_color[i] : 0; }

static void GetSysColor(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret32(c, n, user_sys_color((int)a[0]));
}

static void SetSysColors(CPU *c)       /* void SetSysColors(int n, const int FAR *, const COLORREF FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wll", a);
    for (uint32_t i = 0; i < a[0]; i++) {
        int idx = (int16_t)rd16(FAR_SEL(a[1]), (uint16_t)(FAR_OFF(a[1]) + 2 * i));
        if (idx >= 0 && idx < 21)
            g_sys_color[idx] = rd32(FAR_SEL(a[2]), (uint16_t)(FAR_OFF(a[2]) + 4 * i));
    }
    api_ret(c, n);
}

static void GetDoubleClickTime(CPU *c) { c->a.x = 452; api_ret(c, 0); }
/* Director asks for this over and over while an editable text field has the focus */
static void GetCaretBlinkTime(CPU *c) { video_caret_polled(); c->a.x = 530; api_ret(c, 0); }
static void GetDialogBaseUnits(CPU *c) { api_ret32(c, 0, 8 | 16u << 16); }
static void GetMenuCheckMarkDimensions(CPU *c) { api_ret32(c, 0, 13 | 13u << 16); }

static void MessageBeep(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret(c, n);
}

static void ExitWindows(CPU *c)        /* BOOL ExitWindows(DWORD, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    LOGI("ExitWindows: quitting");
    exit(0);
    api_ret(c, n);
}

static void WinHelp(CPU *c)            /* BOOL WinHelp(HWND, LPCSTR, UINT, DWORD) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wlwl", a);
    c->a.x = 0;
    api_ret(c, n);
}

/* ---- cursors and icons ------------------------------------------------------------------ */

uint16_t g_cursor;
int g_cursor_show;

void user_set_cursor(uint16_t h)
{
    if (h != g_cursor) {
        g_cursor = h;
        video_dirty();
    }
}

static uint16_t new_cursor(int w, int h, int hx, int hy)
{
    Cursor *cur = calloc(1, sizeof(Cursor));
    cur->w = w;
    cur->h = h;
    cur->hot_x = hx;
    cur->hot_y = hy;
    cur->and_mask = calloc((size_t)w * h, 1);
    cur->xor_mask = calloc((size_t)w * h, 1);
    memset(cur->and_mask, 1, (size_t)w * h);
    return h_alloc(HT_CURSOR, cur);
}

/* built-in cursors drawn from text: '#' black, '.' white, ' ' transparent */
static uint16_t cursor_from_art(const char *const *rows, int hx, int hy)
{
    int h = 0, w = 0;
    while (rows[h]) {
        int l = (int)strlen(rows[h]);
        if (l > w) w = l;
        h++;
    }
    uint16_t hc = new_cursor(w, h, hx, hy);
    Cursor *cur = h_get(hc, HT_CURSOR);
    for (int y = 0; y < h; y++)
        for (int x = 0; rows[y][x]; x++) {
            char ch = rows[y][x];
            if (ch == '#' || ch == '.') {
                cur->and_mask[y * w + x] = 0;
                cur->xor_mask[y * w + x] = ch == '.';
            }
        }
    return hc;
}

static const char *const g_arrow_art[] = {
    "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#", "#.......#", "#........#",
    "#.....#####", "#..#..#", "#.# #..#", "##  #..#", "#    #..#", "     #..#", "      #..#", "      ##", NULL
};
static const char *const g_wait_art[] = {
    "###########", "#.........#", " #.......# ", " #.......# ", "  #.....#  ", "   #...#   ", "    #.#    ",
    "   #...#   ", "  #.....#  ", " #.......# ", " #.......# ", "#.........#", "###########", NULL
};
static const char *const g_cross_art[] = {
    "    #    ", "    #    ", "    #    ", "    #    ", "#########", "    #    ", "    #    ", "    #    ",
    "    #    ", NULL
};
static const char *const g_ibeam_art[] = {
    "### ###", "   #   ", "   #   ", "   #   ", "   #   ", "   #   ", "   #   ", "   #   ", "   #   ",
    "   #   ", "   #   ", "   #   ", "   #   ", "   #   ", "### ###", NULL
};

uint16_t user_load_system_cursor(int id)
{
    static struct { int id; uint16_t h; } cache[16];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].id == id)
            return cache[i].h;
    uint16_t h;
    switch (id) {
    case 32513: h = cursor_from_art(g_ibeam_art, 3, 7); break;       /* IDC_IBEAM */
    case 32514: h = cursor_from_art(g_wait_art, 5, 6); break;        /* IDC_WAIT */
    case 32515: h = cursor_from_art(g_cross_art, 4, 4); break;       /* IDC_CROSS */
    default: h = cursor_from_art(g_arrow_art, 0, 0); break;
    }
    if (n < 16) {
        cache[n].id = id;
        cache[n++].h = h;
    }
    return h;
}

/* A DIB cursor/icon image: BITMAPINFOHEADER with doubled height, colour table, XOR image,
 * AND mask (both bottom-up). */
static uint16_t cursor_from_dib(const uint8_t *d, uint32_t size, int hx, int hy, int icon)
{
    int w, h2, bpp, nc, hs, es;
    dib_info(d, &w, &h2, &bpp, &nc, &hs, &es);
    int h = h2 / 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256)
        return 0;
    const uint8_t *table = d + hs;
    const uint8_t *xor_bits = table + nc * es;
    int xor_stride = ((w * bpp + 31) / 32) * 4, and_stride = ((w + 31) / 32) * 4;
    const uint8_t *and_bits = xor_bits + (size_t)xor_stride * h;
    if ((uint32_t)(and_bits + (size_t)and_stride * h - d) > size)
        return 0;
    uint16_t hc = new_cursor(w, h, hx, hy);
    Cursor *cur = h_get(hc, HT_CURSOR);
    uint8_t map[256];
    if (icon || bpp > 1) {
        cur->color = 1;
        cur->color_bits = calloc((size_t)w * h, 1);
        dib_color_map(NULL, table, nc, es, 0, 0, map);
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *xr = xor_bits + (size_t)(h - 1 - y) * xor_stride;
        const uint8_t *ar = and_bits + (size_t)(h - 1 - y) * and_stride;
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            cur->and_mask[i] = (ar[x >> 3] >> (7 - (x & 7))) & 1;
            int v = bpp == 1 ? (xr[x >> 3] >> (7 - (x & 7))) & 1 : bpp == 4 ? (xr[x >> 1] >> ((x & 1) ? 0 : 4)) & 15
                  : bpp == 8 ? xr[x] : 0;
            if (cur->color)
                cur->color_bits[i] = map[v & 255];
            else
                cur->xor_mask[i] = (uint8_t)v;
        }
    }
    return hc;
}

/* Load the first image of a RT_GROUP_CURSOR / RT_GROUP_ICON resource. */
static uint16_t load_cursor_res(uint16_t hinst, uint32_t name, int icon)
{
    LoadedModule *m = module_by_handle(hinst);
    if (!m)
        return 0;
    uint32_t size;
    const uint8_t *dir = res_find(m, res_int(icon ? RT_GROUP_ICON : RT_GROUP_CURSOR), res_id_guest(name), &size);
    if (!dir || size < 6 + 14 || rd16_host(dir + 4) == 0)
        return 0;
    uint16_t id = rd16_host(dir + 6 + 12);          /* first entry's resource ordinal */
    const uint8_t *img = res_find(m, res_int(icon ? RT_ICON : RT_CURSOR), res_int(id), &size);
    if (!img)
        return 0;
    if (icon)
        return cursor_from_dib(img, size, 16, 16, 1);
    return cursor_from_dib(img + 4, size - 4, rd16_host(img), rd16_host(img + 2), 0);
}

static void LoadCursor(CPU *c)         /* HCURSOR LoadCursor(HINSTANCE, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    uint16_t h = 0;
    if (!a[0] && FAR_SEL(a[1]) == 0)
        h = user_load_system_cursor(FAR_OFF(a[1]));
    else
        h = load_cursor_res((uint16_t)a[0], a[1], 0);
    if (!h)
        LOGD("LoadCursor(%04x, %s) failed", a[0], FAR_SEL(a[1]) ? guest_ptr(a[1]) : "#");
    c->a.x = h;
    api_ret(c, n);
}

static void LoadIcon(CPU *c)           /* HICON LoadIcon(HINSTANCE, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    uint16_t h = a[0] ? load_cursor_res((uint16_t)a[0], a[1], 1) : 0;
    if (!h)
        h = user_load_system_cursor(32512);        /* stand-in for the stock icons */
    c->a.x = h;
    api_ret(c, n);
}

/* HCURSOR CreateCursor(HINSTANCE, int xhot, int yhot, int w, int h, const void FAR *and, const void FAR *xor) */
static void CreateCursor(CPU *c)
{
    uint32_t a[7];
    uint16_t n = api_args(c, "wwwwwll", a);
    int w = (int16_t)a[3], h = (int16_t)a[4];
    uint16_t hc = new_cursor(w, h, (int16_t)a[1], (int16_t)a[2]);
    Cursor *cur = h_get(hc, HT_CURSOR);
    const uint8_t *am = (const uint8_t *)guest_ptr(a[5]), *xm = (const uint8_t *)guest_ptr(a[6]);
    int stride = ((w + 15) / 16) * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            cur->and_mask[y * w + x] = (am[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            cur->xor_mask[y * w + x] = (xm[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
        }
    c->a.x = hc;
    api_ret(c, n);
}

static void DestroyCursor(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Cursor *cur = h_get((uint16_t)a[0], HT_CURSOR);
    c->a.x = 0;
    if (cur && (uint16_t)a[0] != g_cursor) {
        free(cur->and_mask);
        free(cur->xor_mask);
        free(cur->color_bits);
        free(cur);
        h_free((uint16_t)a[0]);
        c->a.x = 1;
    }
    api_ret(c, n);
}

static void SetCursor(CPU *c)          /* HCURSOR SetCursor(HCURSOR) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = g_cursor;
    user_set_cursor((uint16_t)a[0]);
    api_ret(c, n);
}

static void GetCursor(CPU *c)
{
    c->a.x = g_cursor;
    api_ret(c, 0);
}

static void ShowCursor(CPU *c)         /* int ShowCursor(BOOL) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    g_cursor_show += a[0] ? 1 : -1;
    video_dirty();
    c->a.x = (uint16_t)g_cursor_show;
    api_ret(c, n);
}

static void GetCursorPos(CPU *c)       /* void GetCursorPos(POINT FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    user_yield();
    wr16(FAR_SEL(a[0]), FAR_OFF(a[0]), (uint16_t)g_mouse_x);
    wr16(FAR_SEL(a[0]), FAR_OFF(a[0]) + 2, (uint16_t)g_mouse_y);
    api_ret(c, n);
}

static void SetCursorPos(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    g_mouse_x = (int16_t)a[0];
    g_mouse_y = (int16_t)a[1];
    video_dirty();
    api_ret(c, n);
}

static void ClipCursor(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    api_ret(c, n);
}

static void DrawIcon(CPU *c)           /* BOOL DrawIcon(HDC, int x, int y, HICON) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwww", a);
    DC *dc = dc_get((uint16_t)a[0]);
    Cursor *cur = h_get((uint16_t)a[3], HT_CURSOR);
    Target t;
    if (dc && cur && dc_target(dc, &t)) {
        int x0 = (int16_t)a[1] + t.ox, y0 = (int16_t)a[2] + t.oy;
        for (int y = 0; y < cur->h; y++)
            for (int x = 0; x < cur->w; x++) {
                int i = y * cur->w + x;
                if (cur->and_mask[i])
                    continue;
                uint8_t v = cur->color ? cur->color_bits[i] : cur->xor_mask[i] ? 255 : 0;
                blit_fill(&t, irect(x0 + x, y0 + y, x0 + x + 1, y0 + y + 1), v);
            }
    }
    c->a.x = 1;
    api_ret(c, n);
}

/* ---- string and bitmap resources ---------------------------------------------------------- */

static void LoadString(CPU *c)         /* int LoadString(HINSTANCE, UINT id, LPSTR, int) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwlw", a);
    LoadedModule *m = module_by_handle((uint16_t)a[0]);
    uint16_t id = (uint16_t)a[1];
    int len = 0;
    uint32_t size;
    const uint8_t *blk = m ? res_find(m, res_int(RT_STRING), res_int((uint16_t)((id >> 4) + 1)), &size) : NULL;
    if (blk) {
        const uint8_t *p = blk;
        for (int i = 0; i < (id & 15) && p < blk + size; i++)
            p += 1 + p[0];
        if (p < blk + size) {
            len = p[0];
            int cap = (int16_t)a[3];
            if (cap > 0) {
                if (len > cap - 1)
                    len = cap - 1;
                memcpy(guest_ptr(a[2]), p + 1, (size_t)len);
                guest_ptr(a[2])[len] = 0;
            }
        }
    } else if ((int16_t)a[3] > 0) {
        guest_ptr(a[2])[0] = 0;
    }
    LOGD("LoadString(%u) = \"%s\"", id, (int16_t)a[3] > 0 ? guest_ptr(a[2]) : "");
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

static void LoadBitmap(CPU *c)         /* HBITMAP LoadBitmap(HINSTANCE, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    LoadedModule *m = module_by_handle((uint16_t)a[0]);
    uint32_t size;
    const uint8_t *d = m && a[0] ? res_find(m, res_int(RT_BITMAP), res_id_guest(a[1]), &size) : NULL;
    uint16_t hb = 0;
    if (d) {
        int w, h, bpp, nc, hs, es;
        dib_info(d, &w, &h, &bpp, &nc, &hs, &es);
        Surface s;
        if (dib_decode_host(NULL, d, NULL, 0, bpp == 1, &s)) {
            hb = gdi_new_bitmap(s.w, s.h, bpp == 1);
            Bitmap *bm = h_get(hb, HT_BITMAP);
            memcpy(bm->s.bits, s.bits, (size_t)s.w * s.h);
            free(s.bits);
        }
    } else if (!a[0]) {
        hb = gdi_new_bitmap(16, 16, 0);            /* OBM_xxx stand-in */
    }
    c->a.x = hb;
    api_ret(c, n);
}

static void LoadAccelerators(CPU *c)   /* HACCEL LoadAccelerators(HINSTANCE, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    static uint16_t dummy;
    if (!dummy)
        dummy = h_alloc(HT_ACCEL, NULL);
    c->a.x = dummy;
    api_ret(c, n);
}

static void TranslateAccelerator(CPU *c)   /* int TranslateAccelerator(HWND, HACCEL, MSG FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    c->a.x = 0;
    api_ret(c, n);
}

/* ---- MessageBox ------------------------------------------------------------------------------- */

static void MessageBox(CPU *c)         /* int MessageBox(HWND, LPCSTR text, LPCSTR caption, UINT type) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wllw", a);
    const char *text = a[1] ? guest_ptr(a[1]) : "";
    const char *caption = a[2] ? guest_ptr(a[2]) : "Error";
    LOGI("MessageBox(\"%s\", \"%s\", %04x)", caption, text, a[3]);
    static const struct { int n; int ids[3]; const char *names[3]; } kinds[] = {
        { 1, { 1 }, { "OK" } },
        { 2, { 1, 2 }, { "OK", "Cancel" } },
        { 3, { 3, 4, 5 }, { "Abort", "Retry", "Ignore" } },
        { 3, { 6, 7, 2 }, { "Yes", "No", "Cancel" } },
        { 2, { 6, 7 }, { "Yes", "No" } },
        { 2, { 4, 2 }, { "Retry", "Cancel" } },
    };
    int kind = (int)(a[3] & 15);
    if (kind > 5)
        kind = 0;
    SDL_MessageBoxButtonData buttons[3];
    for (int i = 0; i < kinds[kind].n; i++) {
        buttons[i].flags = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
        if (kinds[kind].ids[i] == 2)
            buttons[i].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        buttons[i].buttonID = kinds[kind].ids[i];
        buttons[i].text = kinds[kind].names[i];
    }
    int icon = (int)(a[3] & 0xF0);
    SDL_MessageBoxData box = { icon == 0x10 ? SDL_MESSAGEBOX_ERROR : icon == 0x30 ? SDL_MESSAGEBOX_WARNING
                                                                                  : SDL_MESSAGEBOX_INFORMATION,
                               NULL, caption, text, kinds[kind].n, buttons, NULL };
    int result = kinds[kind].ids[0];
    if (!SDL_ShowMessageBox(&box, &result)) {     /* none here (the Switch): show it, take the default */
        LOGW("MessageBox: %s", SDL_GetError());
        host_message(icon == 0x10, text);
    }
    c->a.x = (uint16_t)result;
    api_ret(c, n);
}

/* ---- menus (data only) ------------------------------------------------------------------------ */

typedef struct MenuItem {
    uint16_t flags, id, popup;
    char text[64];
} MenuItem;

typedef struct Menu {
    MenuItem *items;
    int n;
} Menu;

#define MF_BYPOSITION 0x0400
#define MF_POPUP 0x0010
#define MF_SEPARATOR 0x0800
#define MF_CHECKED 0x0008
#define MF_GRAYED 0x0001
#define MF_DISABLED 0x0002
#define MF_END 0x0080

static uint16_t menu_new(void) { return h_alloc(HT_MENU, calloc(1, sizeof(Menu))); }
static Menu *menu_get(uint16_t h) { return h_get(h, HT_MENU); }

static int menu_find(Menu *m, uint16_t item, uint16_t flags, Menu **owner)
{
    if (!m)
        return -1;
    if (flags & MF_BYPOSITION) {
        *owner = m;
        return item < m->n ? item : -1;
    }
    for (int i = 0; i < m->n; i++) {
        if (!(m->items[i].flags & MF_POPUP) && m->items[i].id == item) {
            *owner = m;
            return i;
        }
        if (m->items[i].flags & MF_POPUP) {
            int r = menu_find(menu_get(m->items[i].popup), item, flags, owner);
            if (r >= 0)
                return r;
        }
    }
    return -1;
}

static void menu_insert(Menu *m, int pos, uint16_t flags, uint16_t id, uint32_t text)
{
    if (pos < 0 || pos > m->n)
        pos = m->n;
    m->items = realloc(m->items, sizeof(MenuItem) * (size_t)(m->n + 1));
    memmove(&m->items[pos + 1], &m->items[pos], sizeof(MenuItem) * (size_t)(m->n - pos));
    MenuItem *it = &m->items[pos];
    memset(it, 0, sizeof(*it));
    it->flags = flags & ~MF_BYPOSITION;
    if (flags & MF_POPUP)
        it->popup = id;
    else
        it->id = id;
    if (!(flags & (MF_SEPARATOR | 0x0004 /* MF_BITMAP */ | 0x0100 /* MF_OWNERDRAW */)) && text && FAR_SEL(text))
        snprintf(it->text, sizeof(it->text), "%s", guest_ptr(text));
    m->n++;
}

static const uint8_t *menu_parse(Menu *m, const uint8_t *p, const uint8_t *end)
{
    while (p + 2 <= end) {
        uint16_t flags = rd16_host(p);
        p += 2;
        uint16_t id = 0, popup = 0;
        if (!(flags & MF_POPUP)) {
            id = rd16_host(p);
            p += 2;
        }
        const char *text = (const char *)p;
        p += strlen(text) + 1;
        if (flags & MF_POPUP) {
            popup = menu_new();
            p = menu_parse(menu_get(popup), p, end);
        }
        m->items = realloc(m->items, sizeof(MenuItem) * (size_t)(m->n + 1));
        MenuItem *it = &m->items[m->n++];
        memset(it, 0, sizeof(*it));
        it->flags = flags & ~MF_END;
        it->id = id;
        it->popup = popup;
        snprintf(it->text, sizeof(it->text), "%s", text);
        if (flags & MF_END)
            break;
    }
    return p;
}

static void LoadMenu(CPU *c)           /* HMENU LoadMenu(HINSTANCE, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    LoadedModule *m = module_by_handle((uint16_t)a[0]);
    uint32_t size;
    const uint8_t *d = m ? res_find(m, res_int(RT_MENU), res_id_guest(a[1]), &size) : NULL;
    c->a.x = 0;
    if (d) {
        c->a.x = menu_new();
        menu_parse(menu_get(c->a.x), d + 4 + rd16_host(d + 2), d + size);
    }
    api_ret(c, n);
}

static void CreateMenu(CPU *c) { c->a.x = menu_new(); api_ret(c, 0); }
static void CreatePopupMenu(CPU *c) { c->a.x = menu_new(); api_ret(c, 0); }

static void menu_free(uint16_t h)
{
    Menu *m = menu_get(h);
    if (!m)
        return;
    for (int i = 0; i < m->n; i++)
        if (m->items[i].flags & MF_POPUP)
            menu_free(m->items[i].popup);
    free(m->items);
    free(m);
    h_free(h);
}

static void DestroyMenu(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    menu_free((uint16_t)a[0]);
    c->a.x = 1;
    api_ret(c, n);
}

static void AppendMenu(CPU *c)         /* BOOL AppendMenu(HMENU, UINT flags, UINT id, LPCSTR) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    Menu *m = menu_get((uint16_t)a[0]);
    if (m)
        menu_insert(m, -1, (uint16_t)a[1], (uint16_t)a[2], a[3]);
    c->a.x = m != NULL;
    api_ret(c, n);
}

static void InsertMenu(CPU *c)         /* BOOL InsertMenu(HMENU, UINT pos, UINT flags, UINT id, LPCSTR) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwwl", a);
    Menu *m = menu_get((uint16_t)a[0]), *owner = m;
    int pos = -1;
    if (m && (int16_t)a[1] != -1) {
        pos = menu_find(m, (uint16_t)a[1], (uint16_t)a[2], &owner);
        if (pos < 0)
            owner = m;
    }
    if (m)
        menu_insert(owner, pos, (uint16_t)a[2], (uint16_t)a[3], a[4]);
    c->a.x = m != NULL;
    api_ret(c, n);
}

static void ModifyMenu(CPU *c)         /* BOOL ModifyMenu(HMENU, UINT pos, UINT flags, UINT id, LPCSTR) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwwl", a);
    Menu *owner = NULL;
    int i = menu_find(menu_get((uint16_t)a[0]), (uint16_t)a[1], (uint16_t)a[2], &owner);
    if (i >= 0) {
        memmove(&owner->items[i], &owner->items[i + 1], sizeof(MenuItem) * (size_t)(owner->n - i - 1));
        owner->n--;
        menu_insert(owner, i, (uint16_t)a[2], (uint16_t)a[3], a[4]);
    }
    c->a.x = i >= 0;
    api_ret(c, n);
}

static void remove_item(CPU *c, int destroy)  /* (HMENU, UINT, UINT flags) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Menu *owner = NULL;
    int i = menu_find(menu_get((uint16_t)a[0]), (uint16_t)a[1], (uint16_t)a[2], &owner);
    if (i >= 0) {
        if (destroy && (owner->items[i].flags & MF_POPUP))
            menu_free(owner->items[i].popup);
        memmove(&owner->items[i], &owner->items[i + 1], sizeof(MenuItem) * (size_t)(owner->n - i - 1));
        owner->n--;
    }
    c->a.x = i >= 0;
    api_ret(c, n);
}

static void RemoveMenu(CPU *c) { remove_item(c, 0); }
static void DeleteMenu(CPU *c) { remove_item(c, 1); }

static void item_flag(CPU *c, uint16_t mask)  /* CheckMenuItem / EnableMenuItem: previous state */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Menu *owner = NULL;
    int i = menu_find(menu_get((uint16_t)a[0]), (uint16_t)a[1], (uint16_t)a[2], &owner);
    c->a.x = 0xFFFF;
    if (i >= 0) {
        c->a.x = owner->items[i].flags & mask;
        owner->items[i].flags = (uint16_t)((owner->items[i].flags & ~mask) | (a[2] & mask));
    }
    api_ret(c, n);
}

static void CheckMenuItem(CPU *c) { item_flag(c, MF_CHECKED); }
static void EnableMenuItem(CPU *c) { item_flag(c, MF_GRAYED | MF_DISABLED); }

static void GetMenuState(CPU *c)       /* UINT GetMenuState(HMENU, UINT, UINT flags) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Menu *owner = NULL;
    int i = menu_find(menu_get((uint16_t)a[0]), (uint16_t)a[1], (uint16_t)a[2], &owner);
    c->a.x = 0xFFFF;
    if (i >= 0) {
        MenuItem *it = &owner->items[i];
        c->a.x = it->flags & MF_POPUP ? (uint16_t)((menu_get(it->popup) ? menu_get(it->popup)->n : 0) << 8 | (it->flags & 0xFF))
                                      : it->flags;
    }
    api_ret(c, n);
}

static void GetMenuItemCount(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Menu *m = menu_get((uint16_t)a[0]);
    c->a.x = m ? (uint16_t)m->n : 0xFFFF;
    api_ret(c, n);
}

static void GetMenuItemID(CPU *c)      /* UINT GetMenuItemID(HMENU, int pos) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Menu *m = menu_get((uint16_t)a[0]);
    int pos = (int16_t)a[1];
    c->a.x = 0xFFFF;
    if (m && pos >= 0 && pos < m->n && !(m->items[pos].flags & MF_POPUP))
        c->a.x = m->items[pos].id;
    api_ret(c, n);
}

static void GetSubMenu(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Menu *m = menu_get((uint16_t)a[0]);
    int pos = (int16_t)a[1];
    c->a.x = 0;
    if (m && pos >= 0 && pos < m->n && (m->items[pos].flags & MF_POPUP))
        c->a.x = m->items[pos].popup;
    api_ret(c, n);
}

static void GetMenuString(CPU *c)      /* int GetMenuString(HMENU, UINT, LPSTR, int, UINT flags) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwlww", a);
    Menu *owner = NULL;
    int i = menu_find(menu_get((uint16_t)a[0]), (uint16_t)a[1], (uint16_t)a[4], &owner);
    c->a.x = 0;
    if (i >= 0 && (int16_t)a[3] > 0) {
        size_t len = strlen(owner->items[i].text);
        if (len > a[3] - 1)
            len = a[3] - 1;
        memcpy(guest_ptr(a[2]), owner->items[i].text, len);
        guest_ptr(a[2])[len] = 0;
        c->a.x = (uint16_t)len;
    }
    api_ret(c, n);
}

static void GetMenu(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w && !(w->style & WS_CHILD) ? w->hmenu : 0;
    api_ret(c, n);
}

static void SetMenu(CPU *c)            /* BOOL SetMenu(HWND, HMENU) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    if (w && !(w->style & WS_CHILD))
        w->hmenu = (uint16_t)a[1];
    c->a.x = w != NULL;
    api_ret(c, n);
}

static void GetSystemMenu(CPU *c)      /* HMENU GetSystemMenu(HWND, BOOL revert) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    static uint16_t sysmenu;
    if (!sysmenu)
        sysmenu = menu_new();
    c->a.x = sysmenu;
    api_ret(c, n);
}

static void DrawMenuBar(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret(c, n);
}

static void HiliteMenuItem(CPU *c)     /* BOOL HiliteMenuItem(HWND, HMENU, UINT, UINT) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwww", a);
    c->a.x = 1;
    api_ret(c, n);
}

static void TrackPopupMenu(CPU *c)     /* BOOL TrackPopupMenu(HMENU, UINT, int, int, int, HWND, const RECT FAR *) */
{
    uint32_t a[7];
    uint16_t n = api_args(c, "wwwwwwl", a);
    c->a.x = 0;
    api_ret(c, n);
}

/* ---- clipboard (in-process only) ---------------------------------------------------------- */

static struct { uint16_t format, handle; } g_clip[16];
static int g_nclip;

static void OpenClipboard(CPU *c) { uint32_t a[1]; uint16_t n = api_args(c, "w", a); c->a.x = 1; api_ret(c, n); }
static void CloseClipboard(CPU *c) { c->a.x = 1; api_ret(c, 0); }
static void EmptyClipboard(CPU *c) { g_nclip = 0; c->a.x = 1; api_ret(c, 0); }

static void SetClipboardData(CPU *c)   /* HANDLE SetClipboardData(UINT, HANDLE) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    int i;
    for (i = 0; i < g_nclip && g_clip[i].format != (uint16_t)a[0]; i++)
        ;
    if (i < 16) {
        g_clip[i].format = (uint16_t)a[0];
        g_clip[i].handle = (uint16_t)a[1];
        if (i == g_nclip)
            g_nclip++;
    }
    c->a.x = (uint16_t)a[1];
    api_ret(c, n);
}

static void GetClipboardData(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    for (int i = 0; i < g_nclip; i++)
        if (g_clip[i].format == (uint16_t)a[0])
            c->a.x = g_clip[i].handle;
    api_ret(c, n);
}

static void EnumClipboardFormats(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    for (int i = 0; i < g_nclip; i++)
        if (!a[0] || g_clip[i].format == (uint16_t)a[0]) {
            c->a.x = a[0] ? (i + 1 < g_nclip ? g_clip[i + 1].format : 0) : g_clip[0].format;
            break;
        }
    api_ret(c, n);
}

static void IsClipboardFormatAvailable(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    for (int i = 0; i < g_nclip; i++)
        if (g_clip[i].format == (uint16_t)a[0])
            c->a.x = 1;
    api_ret(c, n);
}

static void CountClipboardFormats(CPU *c) { c->a.x = (uint16_t)g_nclip; api_ret(c, 0); }

static void SetClipboardViewer(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void ChangeClipboardChain(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = 0;
    api_ret(c, n);
}

/* ---- atoms -------------------------------------------------------------------------------- */

static char g_atom[256][64];

static uint16_t atom_add(const char *s, int add)
{
    if (s[0] == '#')
        return (uint16_t)atoi(s + 1);
    for (int i = 0; i < 256; i++)
        if (g_atom[i][0] && !SDL_strcasecmp(g_atom[i], s))
            return (uint16_t)(0xC000 + i);
    if (!add)
        return 0;
    for (int i = 0; i < 256; i++)
        if (!g_atom[i][0]) {
            snprintf(g_atom[i], sizeof(g_atom[i]), "%s", s);
            return (uint16_t)(0xC000 + i);
        }
    return 0;
}

static void GlobalAddAtom(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = FAR_SEL(a[0]) ? atom_add(guest_ptr(a[0]), 1) : FAR_OFF(a[0]);
    api_ret(c, n);
}

static void GlobalFindAtom(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = FAR_SEL(a[0]) ? atom_add(guest_ptr(a[0]), 0) : FAR_OFF(a[0]);
    api_ret(c, n);
}

static void GlobalDeleteAtom(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    if (a[0] >= 0xC000 && a[0] < 0xC100)
        g_atom[a[0] - 0xC000][0] = 0;
    c->a.x = 0;
    api_ret(c, n);
}

static void GlobalGetAtomName(CPU *c)  /* UINT GlobalGetAtomName(ATOM, LPSTR, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    char buf[64] = "";
    if (a[0] >= 0xC000 && a[0] < 0xC100)
        snprintf(buf, sizeof(buf), "%s", g_atom[a[0] - 0xC000]);
    else
        snprintf(buf, sizeof(buf), "#%u", a[0]);
    size_t len = strlen(buf);
    if (a[2] && len > a[2] - 1)
        len = a[2] - 1;
    if (a[2]) {
        memcpy(guest_ptr(a[1]), buf, len);
        guest_ptr(a[1])[len] = 0;
    }
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

static void RegisterWindowMessage(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = atom_add(guest_ptr(a[0]), 1);
    api_ret(c, n);
}

static void RegisterClipboardFormat(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = atom_add(guest_ptr(a[0]), 1);
    api_ret(c, n);
}

/* ---- carets, scroll bars, dialogs, MDI ------------------------------------------------------ */

#define STUB(name, sig, ret)                                                                    \
    static void name(CPU *c)                                                                    \
    {                                                                                           \
        uint32_t a[8];                                                                          \
        uint16_t n = api_args(c, sig, a);                                                       \
        (void)a;                                                                                \
        c->a.x = (uint16_t)(ret);                                                               \
        api_ret(c, n);                                                                          \
    }

STUB(CreateCaret, "wwww", 0)
STUB(DestroyCaret, "", 0)
STUB(SetCaretPos, "ww", 0)
STUB(HideCaret, "w", 0)
STUB(ShowCaret, "w", 0)
STUB(SetCaretBlinkTime, "w", 0)
STUB(SetScrollPos, "wwww", 0)
STUB(GetScrollPos, "ww", 0)
STUB(SetScrollRange, "wwwww", 0)
STUB(ShowScrollBar, "www", 0)
STUB(IsDialogMessage, "wl", 0)
STUB(TranslateMDISysAccel, "wl", 0)
STUB(DragAcceptFiles, "ww", 0)
STUB(DragFinish, "w", 0)

static void GetScrollRange(CPU *c)     /* void GetScrollRange(HWND, int, int FAR *, int FAR *) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwll", a);
    wr16(FAR_SEL(a[2]), FAR_OFF(a[2]), 0);
    wr16(FAR_SEL(a[3]), FAR_OFF(a[3]), 0);
    api_ret(c, n);
}

static void SendDlgItemMessage(CPU *c) /* LRESULT SendDlgItemMessage(HWND, int, UINT, WPARAM, LPARAM) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwwl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    uint32_t r = 0;
    for (uint16_t h = w ? w->child : 0; h; h = wnd_get(h)->next)
        if (wnd_get(h)->id == (uint16_t)a[1]) {
            r = wnd_send(h, (uint16_t)a[2], (uint16_t)a[3], a[4]);
            break;
        }
    api_ret32(c, n, r);
}

static uint16_t dlg_item(uint16_t hdlg, uint16_t id)
{
    Wnd *w = wnd_get(hdlg);
    for (uint16_t h = w ? w->child : 0; h; h = wnd_get(h)->next)
        if (wnd_get(h)->id == id)
            return h;
    return 0;
}

static void SetDlgItemText(CPU *c)     /* void SetDlgItemText(HWND, int, LPCSTR) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    uint16_t h = dlg_item((uint16_t)a[0], (uint16_t)a[1]);
    if (h)
        wnd_send(h, WM_SETTEXT, 0, a[2]);
    api_ret(c, n);
}

static void GetDlgItemText(CPU *c)     /* int GetDlgItemText(HWND, int, LPSTR, int) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwlw", a);
    uint16_t h = dlg_item((uint16_t)a[0], (uint16_t)a[1]);
    c->a.x = 0;
    if (h)
        c->a.x = (uint16_t)wnd_send(h, WM_GETTEXT, (uint16_t)a[3], a[2]);
    else if (a[3])
        guest_ptr(a[2])[0] = 0;
    api_ret(c, n);
}

static void IsDlgButtonChecked(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    uint16_t h = dlg_item((uint16_t)a[0], (uint16_t)a[1]);
    c->a.x = h ? (uint16_t)wnd_send(h, 0x0400 + 0 /* BM_GETCHECK */, 0, 0) : 0;
    api_ret(c, n);
}

/* Dialogs: not implemented yet. Say so loudly, and behave as if the user cancelled. */
static void DialogBox(CPU *c)          /* int DialogBox(HINSTANCE, LPCSTR, HWND, DLGPROC) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wlwl", a);
    LOGW("DialogBox(%s): dialogs are not implemented; returning IDCANCEL",
         FAR_SEL(a[1]) ? guest_ptr(a[1]) : "#");
    c->a.x = 2;
    api_ret(c, n);
}

static void CreateDialog(CPU *c)       /* HWND CreateDialog(HINSTANCE, LPCSTR, HWND, DLGPROC) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wlwl", a);
    LOGW("CreateDialog(%s): dialogs are not implemented", FAR_SEL(a[1]) ? guest_ptr(a[1]) : "#");
    c->a.x = 0;
    api_ret(c, n);
}

static void EndDialog(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    api_ret(c, n);
}

/* DefFrameProc(HWND, HWND client, UINT, WPARAM, LPARAM), DefMDIChildProc(HWND, UINT, WPARAM, LPARAM) */
static void DefFrameProc(CPU *c)
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwwwl", a);
    api_ret32(c, n, def_window_proc((uint16_t)a[0], (uint16_t)a[2], (uint16_t)a[3], a[4]));
}

static void DefMDIChildProc(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    api_ret32(c, n, def_window_proc((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]));
}

/* ---- KEYBOARD and SHELL bits ------------------------------------------------------------- */

static void AnsiToOem(CPU *c)          /* int AnsiToOem(LPCSTR, LPSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    const char *s = guest_ptr(a[0]);
    memmove(guest_ptr(a[1]), s, strlen(s) + 1);
    c->a.x = 1;
    api_ret(c, n);
}

static void AnsiToOemBuff(CPU *c)      /* void AnsiToOemBuff(LPCSTR, LPSTR, UINT) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "llw", a);
    memmove(guest_ptr(a[1]), guest_ptr(a[0]), a[2] ? a[2] : 65536);
    api_ret(c, n);
}

/* int ToAscii(UINT vk, UINT scan, BYTE FAR *state, DWORD FAR *out, UINT flags) */
static void ToAscii(CPU *c)
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwllw", a);
    const uint8_t *state = (const uint8_t *)guest_ptr(a[2]);
    int ch = user_vk_to_char((int)a[0], state);
    LOGD("ToAscii(vk %02x, scan %02x, shift %02x ctrl %02x alt %02x) = %02x", a[0], a[1], state[0x10], state[0x11],
         state[0x12], ch);
    c->a.x = 0;
    if (ch) {
        wr16(FAR_SEL(a[3]), FAR_OFF(a[3]), (uint16_t)ch);
        c->a.x = 1;
    }
    api_ret(c, n);
}

static void DragQueryFile(CPU *c)      /* UINT DragQueryFile(HDROP, UINT, LPSTR, UINT) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwlw", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void FindExecutable(CPU *c)     /* HINSTANCE FindExecutable(LPCSTR, LPCSTR, LPSTR) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lll", a);
    guest_ptr(a[2])[0] = 0;
    c->a.x = 31;                                   /* SE_ERR_NOASSOC */
    api_ret(c, n);
}

const ApiDef g_user_misc_api[] = {
    { "USER", 1, "MessageBox", MessageBox },
    { "USER", 7, "ExitWindows", ExitWindows },
    { "USER", 16, "ClipCursor", ClipCursor },
    { "USER", 17, "GetCursorPos", GetCursorPos },
    { "USER", 21, "GetDoubleClickTime", GetDoubleClickTime },
    { "USER", 62, "SetScrollPos", SetScrollPos },
    { "USER", 63, "GetScrollPos", GetScrollPos },
    { "USER", 64, "SetScrollRange", SetScrollRange },
    { "USER", 65, "GetScrollRange", GetScrollRange },
    { "USER", 69, "SetCursor", SetCursor },
    { "USER", 70, "SetCursorPos", SetCursorPos },
    { "USER", 71, "ShowCursor", ShowCursor },
    { "USER", 72, "SetRect", SetRect },
    { "USER", 73, "SetRectEmpty", SetRectEmpty },
    { "USER", 74, "CopyRect", CopyRect },
    { "USER", 75, "IsRectEmpty", IsRectEmpty },
    { "USER", 76, "PtInRect", PtInRect },
    { "USER", 77, "OffsetRect", OffsetRect },
    { "USER", 78, "InflateRect", InflateRect },
    { "USER", 79, "IntersectRect", IntersectRect },
    { "USER", 80, "UnionRect", UnionRect },
    { "USER", 84, "DrawIcon", DrawIcon },
    { "USER", 87, "DialogBox", DialogBox },
    { "USER", 88, "EndDialog", EndDialog },
    { "USER", 89, "CreateDialog", CreateDialog },
    { "USER", 90, "IsDialogMessage", IsDialogMessage },
    { "USER", 92, "SetDlgItemText", SetDlgItemText },
    { "USER", 93, "GetDlgItemText", GetDlgItemText },
    { "USER", 98, "IsDlgButtonChecked", IsDlgButtonChecked },
    { "USER", 101, "SendDlgItemMessage", SendDlgItemMessage },
    { "USER", 104, "MessageBeep", MessageBeep },
    { "USER", 118, "RegisterWindowMessage", RegisterWindowMessage },
    { "USER", 137, "OpenClipboard", OpenClipboard },
    { "USER", 138, "CloseClipboard", CloseClipboard },
    { "USER", 139, "EmptyClipboard", EmptyClipboard },
    { "USER", 141, "SetClipboardData", SetClipboardData },
    { "USER", 142, "GetClipboardData", GetClipboardData },
    { "USER", 143, "CountClipboardFormats", CountClipboardFormats },
    { "USER", 144, "EnumClipboardFormats", EnumClipboardFormats },
    { "USER", 145, "RegisterClipboardFormat", RegisterClipboardFormat },
    { "USER", 147, "SetClipboardViewer", SetClipboardViewer },
    { "USER", 149, "ChangeClipboardChain", ChangeClipboardChain },
    { "USER", 150, "LoadMenu", LoadMenu },
    { "USER", 151, "CreateMenu", CreateMenu },
    { "USER", 152, "DestroyMenu", DestroyMenu },
    { "USER", 154, "CheckMenuItem", CheckMenuItem },
    { "USER", 155, "EnableMenuItem", EnableMenuItem },
    { "USER", 156, "GetSystemMenu", GetSystemMenu },
    { "USER", 157, "GetMenu", GetMenu },
    { "USER", 158, "SetMenu", SetMenu },
    { "USER", 159, "GetSubMenu", GetSubMenu },
    { "USER", 160, "DrawMenuBar", DrawMenuBar },
    { "USER", 161, "GetMenuString", GetMenuString },
    { "USER", 162, "HiliteMenuItem", HiliteMenuItem },
    { "USER", 163, "CreateCaret", CreateCaret },
    { "USER", 164, "DestroyCaret", DestroyCaret },
    { "USER", 165, "SetCaretPos", SetCaretPos },
    { "USER", 166, "HideCaret", HideCaret },
    { "USER", 167, "ShowCaret", ShowCaret },
    { "USER", 168, "SetCaretBlinkTime", SetCaretBlinkTime },
    { "USER", 169, "GetCaretBlinkTime", GetCaretBlinkTime },
    { "USER", 171, "WinHelp", WinHelp },
    { "USER", 173, "LoadCursor", LoadCursor },
    { "USER", 174, "LoadIcon", LoadIcon },
    { "USER", 175, "LoadBitmap", LoadBitmap },
    { "USER", 176, "LoadString", LoadString },
    { "USER", 177, "LoadAccelerators", LoadAccelerators },
    { "USER", 178, "TranslateAccelerator", TranslateAccelerator },
    { "USER", 179, "GetSystemMetrics", GetSystemMetrics },
    { "USER", 180, "GetSysColor", GetSysColor },
    { "USER", 181, "SetSysColors", SetSysColors },
    { "USER", 193, "IsClipboardFormatAvailable", IsClipboardFormatAvailable },
    { "USER", 243, "GetDialogBaseUnits", GetDialogBaseUnits },
    { "USER", 244, "EqualRect", EqualRect },
    { "USER", 247, "GetCursor", GetCursor },
    { "USER", 250, "GetMenuState", GetMenuState },
    { "USER", 263, "GetMenuItemCount", GetMenuItemCount },
    { "USER", 264, "GetMenuItemID", GetMenuItemID },
    { "USER", 267, "ShowScrollBar", ShowScrollBar },
    { "USER", 268, "GlobalAddAtom", GlobalAddAtom },
    { "USER", 269, "GlobalDeleteAtom", GlobalDeleteAtom },
    { "USER", 270, "GlobalFindAtom", GlobalFindAtom },
    { "USER", 271, "GlobalGetAtomName", GlobalGetAtomName },
    { "USER", 373, "SubtractRect", SubtractRect },
    { "USER", 406, "CreateCursor", CreateCursor },
    { "USER", 410, "InsertMenu", InsertMenu },
    { "USER", 411, "AppendMenu", AppendMenu },
    { "USER", 412, "RemoveMenu", RemoveMenu },
    { "USER", 413, "DeleteMenu", DeleteMenu },
    { "USER", 414, "ModifyMenu", ModifyMenu },
    { "USER", 415, "CreatePopupMenu", CreatePopupMenu },
    { "USER", 416, "TrackPopupMenu", TrackPopupMenu },
    { "USER", 417, "GetMenuCheckMarkDimensions", GetMenuCheckMarkDimensions },
    { "USER", 420, "_wsprintf", wsprintf },
    { "USER", 421, "wvsprintf", wvsprintf },
    { "USER", 430, "lstrcmp", lstrcmp },
    { "USER", 431, "AnsiUpper", AnsiUpper },
    { "USER", 432, "AnsiLower", AnsiLower },
    { "USER", 433, "IsCharAlpha", IsCharAlpha },
    { "USER", 434, "IsCharAlphaNumeric", IsCharAlphaNumeric },
    { "USER", 435, "IsCharUpper", IsCharUpper },
    { "USER", 436, "IsCharLower", IsCharLower },
    { "USER", 437, "AnsiUpperBuff", AnsiUpperBuff },
    { "USER", 438, "AnsiLowerBuff", AnsiLowerBuff },
    { "USER", 445, "DefFrameProc", DefFrameProc },
    { "USER", 447, "DefMDIChildProc", DefMDIChildProc },
    { "USER", 451, "TranslateMDISysAccel", TranslateMDISysAccel },
    { "USER", 457, "DestroyIcon", DestroyCursor },
    { "USER", 458, "DestroyCursor", DestroyCursor },
    { "USER", 471, "lstrcmpi", lstrcmpi },
    { "USER", 472, "AnsiNext", AnsiNext },
    { "USER", 473, "AnsiPrev", AnsiPrev },
    { "KEYBOARD", 4, "ToAscii", ToAscii },
    { "KEYBOARD", 5, "AnsiToOem", AnsiToOem },
    { "KEYBOARD", 6, "OemToAnsi", AnsiToOem },
    { "KEYBOARD", 134, "AnsiToOemBuff", AnsiToOemBuff },
    { "KEYBOARD", 135, "OemToAnsiBuff", AnsiToOemBuff },
    { "SHELL", 9, "DragAcceptFiles", DragAcceptFiles },
    { "SHELL", 11, "DragQueryFile", DragQueryFile },
    { "SHELL", 12, "DragFinish", DragFinish },
    { "SHELL", 21, "FindExecutable", FindExecutable },
    { NULL, 0, NULL, NULL }
};
