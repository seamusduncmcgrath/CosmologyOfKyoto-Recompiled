/* Shared declarations for the Win16 USER and GDI layers (runtime-internal).
 *
 * The emulated machine has one 8-bit palettized screen (g_screen, 640x480, like the VGA
 * 256-colour displays the game targets) that video.c presents in an SDL window. Windows
 * are tracked by USER and drawn into the screen surface through GDI; nothing maps to
 * native windows.
 */
#ifndef WIN_H
#define WIN_H

#include <stdlib.h>
#include <string.h>

#include "rt.h"

static inline uint16_t rd16_host(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32_host(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr16_host(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr32_host(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ---- geometry ---------------------------------------------------------------------------- */

typedef struct IRect {
    int l, t, r, b;
} IRect;

static inline int rect_empty(const IRect *r) { return r->l >= r->r || r->t >= r->b; }
static inline IRect irect(int l, int t, int r, int b) { IRect x = { l, t, r, b }; return x; }
static inline void rect_offset(IRect *r, int dx, int dy) { r->l += dx; r->r += dx; r->t += dy; r->b += dy; }
static inline int rect_has_pt(const IRect *r, int x, int y) { return x >= r->l && x < r->r && y >= r->t && y < r->b; }
int rect_intersect(IRect *out, const IRect *a, const IRect *b);    /* returns !empty */
void rect_union(IRect *out, const IRect *a, const IRect *b);

/* guest RECT and POINT (16-bit fields) */
static inline IRect rd_rect(uint32_t far)
{
    uint16_t s = FAR_SEL(far), o = FAR_OFF(far);
    return irect((int16_t)rd16(s, o), (int16_t)rd16(s, o + 2), (int16_t)rd16(s, o + 4), (int16_t)rd16(s, o + 6));
}
static inline void wr_rect(uint32_t far, IRect r)
{
    uint16_t s = FAR_SEL(far), o = FAR_OFF(far);
    wr16(s, o, (uint16_t)r.l); wr16(s, o + 2, (uint16_t)r.t);
    wr16(s, o + 4, (uint16_t)r.r); wr16(s, o + 6, (uint16_t)r.b);
}

/* ---- handles ------------------------------------------------------------------------------ */

enum { HT_FREE, HT_WND, HT_MENU, HT_CURSOR, HT_DC, HT_BITMAP, HT_PALETTE, HT_BRUSH, HT_PEN, HT_FONT,
       HT_RGN, HT_ACCEL, HT_HOOK };

uint16_t h_alloc(int type, void *obj);
void *h_get(uint16_t h, int type);          /* NULL if not a live handle of that type */
int h_type(uint16_t h);
void h_free(uint16_t h);

/* ---- regions (lists of non-overlapping rectangles) ------------------------------------------ */

typedef struct Region {
    IRect *r;
    int n, cap;
    IRect box;
} Region;

enum { NULLREGION = 1, SIMPLEREGION, COMPLEXREGION };
enum { RGN_AND = 1, RGN_OR, RGN_XOR, RGN_DIFF, RGN_COPY };

void rgn_init(Region *g);
void rgn_free(Region *g);
void rgn_set_rect(Region *g, IRect r);
void rgn_copy(Region *dst, const Region *src);
void rgn_combine(Region *dst, const Region *a, const Region *b, int op);
void rgn_offset(Region *g, int dx, int dy);
void rgn_clip_rect(Region *g, IRect r);    /* g &= r */
int rgn_pt_in(const Region *g, int x, int y);
int rgn_rect_in(const Region *g, IRect r);
int rgn_kind(const Region *g);              /* NULLREGION / SIMPLEREGION / COMPLEXREGION */
int rgn_equal(const Region *a, const Region *b);
/* Build from scanline spans: add rectangles (non-overlapping) then finish. */
void rgn_add(Region *g, IRect r);
void rgn_finish(Region *g);
/* Polygon spans (ALTERNATE = 1, WINDING = 2) and ellipses, as regions. */
void rgn_polygon(Region *g, const int *xy, const int *counts, int npolys, int mode);
void rgn_ellipse(Region *g, IRect r);
void rgn_roundrect(Region *g, IRect r, int ew, int eh);

/* ---- surfaces and the screen ------------------------------------------------------------------ */

/* One byte per pixel. Colour surfaces hold system palette indices, or for a DIB device
 * context the DIB's colour table indices (then `map` translates them to system indices and
 * `inv` back); monochrome ones hold 0x00 (black) or 0xFF (white), so raster operations work
 * bitwise on both. The stride is negative for bottom-up DIBs. */
typedef struct Surface {
    int w, h;
    int mono;
    int stride;
    uint8_t *bits;               /* the top row */
    const uint8_t *map;          /* pixel -> system index, NULL = identity */
    const uint8_t *inv;          /* system index -> pixel, NULL = identity */
} Surface;

static inline uint8_t *surf_row(const Surface *s, int y) { return s->bits + (ptrdiff_t)y * s->stride; }

#define SCREEN_W 640
#define SCREEN_H 480

extern Surface g_screen;
extern uint8_t g_syspal[256][3];            /* the hardware palette */

void video_init(void);
void video_dirty(void);                     /* screen or palette changed */
void video_present(int force);              /* rate-limited unless forced */
/* Poll host events into the input queue; wait up to `wait_ms` for one if none pending. */
void video_pump(int wait_ms);
void video_set_cursor_visible(int on);
struct SDL_Window *video_sdl_window(void);
void video_native_modal(int on);            /* a native dialog is up (keeps the host cursor) */
void video_caret_polled(void);              /* a text field wants typing (GetCaretBlinkTime) */

/* ---- host services SDL doesn't cover everywhere (host.c) --------------------------------------- */

void host_init(void);                       /* first thing at start-up */
int host_has_console(void);                 /* the log already goes somewhere (nxlink) */
void host_message(int error, const char *text);   /* a message box (the Switch: error dialog) */
/* Ask for a line of text (at most max_len characters) with the system keyboard, `sub` shown
 * under the header: 1 typed, 0 cancelled, -1 no such keyboard here. */
int host_text_prompt(const char *header, const char *sub, const char *initial, int max_len, char *out, size_t cap);

/* ---- palettes (gdi_pal.c) --------------------------------------------------------------------- */

#define PC_RESERVED 0x01
#define PC_EXPLICIT 0x02
#define PC_NOCOLLAPSE 0x04

typedef struct Palette {
    int n;
    uint8_t e[256][4];           /* r, g, b, flags */
    uint8_t map[256];            /* logical index -> system palette index */
} Palette;

extern uint16_t g_default_palette;          /* stock DEFAULT_PALETTE */
extern unsigned g_syspal_gen;               /* bumped whenever g_syspal changes */
void pal_init(void);
int pal_nearest(const Palette *p, int r, int g, int b);           /* logical index */
int syspal_nearest_static(int r, int g, int b);                   /* system index */

/* ---- GDI device contexts ------------------------------------------------------------------------ */

enum { DC_SCREEN, DC_MEMORY, DC_INFO, DC_DIB };

typedef struct Bitmap {
    Surface s;
    uint16_t dc;                 /* DC it's selected into, or 0 */
} Bitmap;

typedef struct DC DC;
struct DC {
    int kind;
    uint16_t hwnd;               /* screen DCs: window (0 = whole screen) */
    int whole_window;            /* GetWindowDC: origin at the window rect, not the client */
    int paint;                   /* BeginPaint DC: also clipped to the update region */
    Region paint_rgn;            /* screen coordinates */
    uint16_t hbitmap, hpen, hbrush, hfont, hpalette;
    int has_clip;
    Region clip;                 /* app clip region, device coordinates */
    uint32_t text_color, bk_color;
    int bk_mode, rop2, text_align, stretch_mode, poly_fill_mode, map_mode;
    int vp_x, vp_y, win_x, win_y;
    int cur_x, cur_y;
    int brush_x, brush_y;
    int char_extra, break_extra, break_count;
    DC *saved;                   /* SaveDC stack */
    int nsaved;
    int owned;                   /* CS_OWNDC: belongs to its window, ReleaseDC keeps it */
    /* DC_DIB: draws into a packed DIB in guest memory (DIB.DRV / Director's DIRDIB.DRV) */
    Surface dib;
    uint32_t dib_info;           /* far pointer to its BITMAPINFO */
    unsigned dib_gen;            /* g_syspal_gen the maps were built for */
    uint32_t dib_sig;            /* checksum of the colour table they were built from */
    uint8_t dib_map[256], dib_inv[256];
    int dib_bpp;
    uint8_t *dib_shadow;         /* 1 bpp: byte-per-pixel copy of the bits (see gdi.c) */
    int shadow_live;             /* loaded for the current API call */
    /* cached drawing target (see dc_target) */
    unsigned cache_gen;
    Region cache_clip;
    int cache_ox, cache_oy;
};

/* Where a DC draws: surface, offset from device to surface coordinates, clip region in
 * surface coordinates. */
typedef struct Target {
    Surface *s;
    int ox, oy;
    const Region *clip;
    DC *dc;
} Target;

int dc_target(DC *dc, Target *t);           /* 0 if nothing can be drawn */
DC *dc_get(uint16_t hdc);
extern unsigned g_layout_gen;               /* bumped when window geometry or visibility changes */
uint16_t gdi_screen_dc(uint16_t hwnd, int whole_window);    /* GetDC / GetWindowDC */
void gdi_release_dc(uint16_t hdc);
/* Colour of a COLORREF as a pixel value on the DC's surface. */
uint8_t dc_color(DC *dc, uint32_t colorref);
/* DIB colour table -> system indices (or 0x00/0xFF for a monochrome destination). */
void dib_color_map(DC *dc, const uint8_t *table, int ncolors, int entry_size, int pal_colors, int mono_dst,
                   uint8_t out[256]);
uint8_t dib_rgb_to_sys(DC *dc, int r, int g, int b);
void gdi_init(void);
void gdi_debug_dump_dib(const char *path);
/* Fill helpers used by USER (screen coordinates relative to the DC's origin). */
void gdi_fill_rect(DC *dc, IRect r, uint16_t hbrush);
void gdi_invert_rect(DC *dc, IRect r);
void gdi_frame_rect(DC *dc, IRect r, uint16_t hbrush);
uint16_t gdi_stock(int index);
/* Text for USER (logical coordinates of the DC). */
int gdi_text_extent(DC *dc, const char *s, int n, int *height);
void gdi_text_out(DC *dc, int x, int y, const char *s, int n);
int gdi_draw_text_box(DC *dc, IRect r, const char *text, int align, int wrap, int noprefix, int vcenter);
uint16_t gdi_solid_brush(uint32_t colorref);            /* cached, never deleted */

/* ---- USER ---------------------------------------------------------------------------------- */

typedef struct WndClass {
    char name[64];
    uint16_t style;
    uint32_t wndproc;
    int16_t cls_extra, wnd_extra;
    uint16_t hinst, hicon, hcursor, hbrush;
    char menu_name[64];
    uint8_t *extra;
    uint16_t atom;
    int builtin;
} WndClass;

typedef struct Wnd {
    uint16_t hwnd;
    WndClass *cls;
    uint16_t parent, owner;      /* parent 0 = top level (child of the desktop) */
    uint16_t child, next;        /* first child (topmost), next sibling below */
    uint32_t style, exstyle;
    IRect wr;                    /* window rect, relative to the parent's client area */
    IRect cr;                    /* client rect, same coordinates */
    uint32_t wndproc;
    uint16_t hinst, id, hmenu;
    char *text;
    uint8_t *extra;
    Region update;               /* update region, client coordinates */
    int erase;                   /* WM_ERASEBKGND pending */
    int nc_dirty;                /* WM_NCPAINT pending */
    struct { uint16_t atom; uint16_t value; char name[32]; } props[8];
    int nprops;
    int destroying;
    uint16_t owndc;              /* CS_OWNDC */
    /* built-in controls (user_ctl.c) */
    uint16_t ctl_font;           /* WM_SETFONT */
    uint8_t ctl_check;           /* BM_SETCHECK state */
    uint8_t ctl_pressed, ctl_tracking;
} Wnd;

#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_VSCROLL 0x00200000u
#define WS_HSCROLL 0x00100000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_EX_DLGMODALFRAME 0x0001u
#define WS_EX_TOPMOST 0x0008u

Wnd *wnd_get(uint16_t hwnd);
extern uint16_t g_desktop;
/* Screen rectangles of a window and its client area. */
IRect wnd_screen_rect(Wnd *w);
IRect wnd_screen_client(Wnd *w);
int wnd_visible(Wnd *w);                    /* it and all its ancestors are visible */
/* The region of the screen a window (client area or whole window) may draw into. */
void wnd_vis_region(Wnd *w, int whole_window, Region *out);
uint16_t wnd_from_point(int x, int y);      /* deepest visible, enabled-or-not window */
void wnd_invalidate(Wnd *w, const IRect *client_rect, int erase);   /* NULL = whole client */

/* Messages */
uint32_t wnd_send(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t wnd_call_proc(uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
int msg_post(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t def_window_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
extern CPU *g_cpu;                          /* the guest CPU, for callbacks from the runtime */
extern uint16_t g_focus, g_active, g_capture;

/* Input from video.c */
enum { IN_MOUSE_MOVE, IN_MOUSE_DOWN, IN_MOUSE_UP, IN_KEY_DOWN, IN_KEY_UP, IN_QUIT };
void input_event(int kind, int a, int b, int x, int y);    /* buttons 0..2 / VK + scancode */
extern int g_mouse_x, g_mouse_y;
extern uint8_t g_key_state[256];            /* bit 7 down, bit 0 toggled */

/* Cursors (user_misc.c) */
typedef struct Cursor {
    int w, h, hot_x, hot_y;
    uint8_t *and_mask, *xor_mask;           /* 1 byte per pixel: 0/1 */
    int color;                              /* icon with colour image */
    uint8_t *color_bits;                    /* system indices when color */
} Cursor;
extern uint16_t g_cursor;                   /* current cursor (0 = none) */
extern int g_cursor_show;                   /* ShowCursor count */
void user_set_cursor(uint16_t hcursor);
uint16_t user_load_system_cursor(int id);   /* IDC_ARROW = 32512, ... */
uint32_t user_sys_color(int index);         /* GetSysColor */
void user_kill_window_timers(uint16_t hwnd);
uint16_t user_begin_paint(uint16_t hwnd, int *erase_needed, IRect *paint);
uint16_t user_set_focus(uint16_t hwnd);     /* SetFocus; returns the previous focus */
int user_vk_to_char(int vk, const uint8_t *state);
void user_init(void);
void crash_init(void);                      /* crash.c */
void rt_log_to_file_if_no_console(const char *pref);
void rt_native_backtrace(int max);

/* Window messages used by the runtime */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SYSCOLORCHANGE 0x0015
#define WM_ENDSESSION 0x0016
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_CANCELMODE 0x001F
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_NCMOUSEMOVE 0x00A0
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_INITMENU 0x0116
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_PARENTNOTIFY 0x0210
#define WM_MDIACTIVATE 0x0222
#define WM_QUERYNEWPALETTE 0x030F
#define WM_PALETTEISCHANGING 0x0310
#define WM_PALETTECHANGED 0x0311

#define HTCLIENT 1
#define HTCAPTION 2

#endif
