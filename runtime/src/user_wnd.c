/* USER windows: classes, the window tree, geometry, visibility, activation and focus,
 * painting (update regions, BeginPaint), and DefWindowProc.
 *
 * Every window lives in the virtual screen. A window's rectangles are kept relative to
 * its parent's client area (top-level windows are children of the desktop window, whose
 * client area is the screen). Siblings are linked topmost first. Anything a window
 * procedure needs to see in guest memory (CREATESTRUCT, RECT, WINDOWPOS, ...) is built on
 * the guest stack just below SP for the duration of the call.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi.h"

uint16_t g_desktop;
unsigned g_layout_gen = 1;

Wnd *wnd_get(uint16_t hwnd) { return h_get(hwnd, HT_WND); }

/* ---- guest scratch memory on the stack -------------------------------------------------- */

static uint32_t stack_alloc(uint16_t size)
{
    CPU *c = g_cpu;
    c->sp.x = (uint16_t)(c->sp.x - ((size + 1) & ~1));
    memset(mem_ptr(c->ss, c->sp.x), 0, size);
    return (uint32_t)c->ss << 16 | c->sp.x;
}

/* ---- classes ---------------------------------------------------------------------------- */

#define MAX_CLASSES 64
static WndClass *g_class[MAX_CLASSES];
static uint16_t g_next_atom = 0xC000;

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

static WndClass *class_find(uint32_t name_far, const char *host_name)
{
    for (int i = 0; i < MAX_CLASSES; i++) {
        WndClass *k = g_class[i];
        if (!k)
            continue;
        if (host_name ? ieq(k->name, host_name)
                      : FAR_SEL(name_far) == 0 ? k->atom == FAR_OFF(name_far) : ieq(k->name, guest_ptr(name_far)))
            return k;
    }
    return NULL;
}

static WndClass *class_add(const char *name, uint16_t style, uint32_t proc, int cls_extra, int wnd_extra,
                           uint16_t hinst, uint16_t hcursor, uint16_t hbrush)
{
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (g_class[i])
            continue;
        WndClass *k = calloc(1, sizeof(WndClass));
        snprintf(k->name, sizeof(k->name), "%s", name);
        k->style = style;
        k->wndproc = proc;
        k->cls_extra = (int16_t)cls_extra;
        k->wnd_extra = (int16_t)wnd_extra;
        k->hinst = hinst;
        k->hcursor = hcursor;
        k->hbrush = hbrush;
        k->extra = calloc(1, (size_t)(cls_extra > 0 ? cls_extra : 0) + 4);
        k->atom = g_next_atom++;
        g_class[i] = k;
        return k;
    }
    return NULL;
}

static uint32_t builtin_proc(const char *name)
{
    return api_slot_addr(api_slot("USER", 0, name, name));
}

/* ---- non-client metrics (Windows 3.1 rules, as in Wine's NC_AdjustRect) --------------- */

static int has_thick(uint32_t s) { return (s & WS_THICKFRAME) && (s & (WS_DLGFRAME | WS_BORDER)) != WS_DLGFRAME; }
static int has_dlg(uint32_t s, uint32_t ex) { return (ex & WS_EX_DLGMODALFRAME) || ((s & WS_DLGFRAME) && !(s & WS_BORDER)); }
static int has_thin(uint32_t s) { return (s & WS_BORDER) || !(s & (WS_CHILD | WS_POPUP)); }

/* insets from window rect to client rect */
static IRect nc_insets(uint32_t style, uint32_t ex, int menu)
{
    IRect in = { 0, 0, 0, 0 };
    int f = 0;
    if (style & WS_MINIMIZE)
        return in;
    if (has_thick(style))
        f = 4;
    else if (has_dlg(style, ex))
        f = 4;
    else if (has_thin(style))
        f = 1;
    in.l = in.t = in.r = in.b = f;
    if ((style & WS_CAPTION) == WS_CAPTION)
        in.t += 20 - 1;
    if (menu)
        in.t += 18 + 1;
    if (style & WS_VSCROLL)
        in.r += 17 - 1;
    if (style & WS_HSCROLL)
        in.b += 17 - 1;
    return in;
}

/* ---- the window tree -------------------------------------------------------------------- */

static void unlink_wnd(Wnd *w)
{
    Wnd *p = wnd_get(w->parent ? w->parent : g_desktop);
    if (!p || w->hwnd == g_desktop)
        return;
    if (p->child == w->hwnd) {
        p->child = w->next;
    } else {
        for (uint16_t h = p->child; h; ) {
            Wnd *s = wnd_get(h);
            if (s->next == w->hwnd) {
                s->next = w->next;
                break;
            }
            h = s->next;
        }
    }
    w->next = 0;
}

/* after = 0: top; 1: bottom; else after that sibling */
static void link_wnd(Wnd *w, uint16_t after)
{
    Wnd *p = wnd_get(w->parent ? w->parent : g_desktop);
    if (!p)
        return;
    if (after == 0 || !p->child) {
        w->next = p->child;
        p->child = w->hwnd;
        return;
    }
    uint16_t h = p->child;
    Wnd *s = wnd_get(h);
    if (after == 1) {
        while (s->next)
            s = wnd_get(s->next);
    } else {
        while (s && s->hwnd != after && s->next)
            s = wnd_get(s->next);
    }
    w->next = s->next;
    s->next = w->hwnd;
}

static IRect parent_origin(Wnd *w)
{
    if (!w->parent)
        return irect(0, 0, 0, 0);
    Wnd *p = wnd_get(w->parent);
    IRect pc = p ? wnd_screen_client(p) : irect(0, 0, 0, 0);
    return pc;
}

IRect wnd_screen_rect(Wnd *w)
{
    IRect o = parent_origin(w), r = w->wr;
    rect_offset(&r, o.l, o.t);
    return r;
}

IRect wnd_screen_client(Wnd *w)
{
    if (w->hwnd == g_desktop)
        return irect(0, 0, SCREEN_W, SCREEN_H);
    IRect o = parent_origin(w), r = w->cr;
    rect_offset(&r, o.l, o.t);
    return r;
}

int wnd_visible(Wnd *w)
{
    for (; w; w = w->parent ? wnd_get(w->parent) : NULL)
        if (!(w->style & WS_VISIBLE))
            return 0;
    return 1;
}

void wnd_vis_region(Wnd *w, int whole, Region *out)
{
    if (!wnd_visible(w)) {
        rgn_set_rect(out, irect(0, 0, 0, 0));
        return;
    }
    IRect r = whole ? wnd_screen_rect(w) : wnd_screen_client(w);
    IRect scr = irect(0, 0, SCREEN_W, SCREEN_H);
    rect_intersect(&r, &r, &scr);
    for (Wnd *a = w->parent ? wnd_get(w->parent) : NULL; a; a = a->parent ? wnd_get(a->parent) : NULL) {
        IRect ac = wnd_screen_client(a);
        rect_intersect(&r, &r, &ac);
    }
    rgn_set_rect(out, r);
    Region tmp;
    rgn_init(&tmp);
    /* own children (WS_CLIPCHILDREN) */
    if ((w->style & WS_CLIPCHILDREN) || w->hwnd == g_desktop) {
        for (uint16_t h = w->child; h; ) {
            Wnd *ch = wnd_get(h);
            if (ch->style & WS_VISIBLE) {
                rgn_set_rect(&tmp, wnd_screen_rect(ch));
                rgn_combine(out, out, &tmp, RGN_DIFF);
            }
            h = ch->next;
        }
    }
    /* siblings above, at every level up: top-level windows always clip each other, child
     * windows when WS_CLIPSIBLINGS */
    for (Wnd *x = w; x && x->hwnd != g_desktop; x = x->parent ? wnd_get(x->parent) : wnd_get(g_desktop)) {
        int clip = !x->parent || (x->style & WS_CLIPSIBLINGS) || x != w;
        Wnd *p = wnd_get(x->parent ? x->parent : g_desktop);
        if (!p)
            break;
        for (uint16_t h = p->child; h && h != x->hwnd; ) {
            Wnd *s = wnd_get(h);
            if ((s->style & WS_VISIBLE) && (clip || !s->parent)) {
                rgn_set_rect(&tmp, wnd_screen_rect(s));
                rgn_combine(out, out, &tmp, RGN_DIFF);
            }
            h = s->next;
        }
        if (!x->parent)
            break;
    }
    rgn_free(&tmp);
}

static uint16_t from_point_in(uint16_t first, int x, int y)
{
    for (uint16_t h = first; h; ) {
        Wnd *w = wnd_get(h);
        if ((w->style & WS_VISIBLE)) {
            IRect r = wnd_screen_rect(w);
            if (rect_has_pt(&r, x, y)) {
                IRect cr = wnd_screen_client(w);
                if (!(w->style & WS_MINIMIZE) && rect_has_pt(&cr, x, y)) {
                    /* disabled child windows let input through to their parent */
                    for (uint16_t ch = w->child; ch; ) {
                        Wnd *cw = wnd_get(ch);
                        if (!(cw->style & WS_DISABLED)) {
                            uint16_t hit = from_point_in(ch, x, y);
                            if (hit)
                                return hit;
                            break;
                        }
                        ch = cw->next;
                    }
                    uint16_t hit = from_point_in(w->child, x, y);
                    if (hit && !(wnd_get(hit)->style & WS_DISABLED))
                        return hit;
                }
                return h;
            }
        }
        h = w->next;
    }
    return 0;
}

uint16_t wnd_from_point(int x, int y)
{
    Wnd *d = wnd_get(g_desktop);
    uint16_t h = from_point_in(d ? d->child : 0, x, y);
    return h ? h : g_desktop;
}

static int is_child_of(uint16_t parent, uint16_t h)
{
    for (Wnd *w = wnd_get(h); w && w->parent; w = wnd_get(w->parent))
        if (w->parent == parent)
            return 1;
    return 0;
}

/* ---- update regions ----------------------------------------------------------------------- */

void wnd_invalidate(Wnd *w, const IRect *r, int erase)
{
    if (!w)
        return;
    IRect cr = wnd_screen_client(w);
    IRect area = irect(0, 0, cr.r - cr.l, cr.b - cr.t);
    if (r)
        rect_intersect(&area, &area, r);
    if (rect_empty(&area))
        return;
    Region tmp;
    rgn_init(&tmp);
    rgn_set_rect(&tmp, area);
    rgn_combine(&w->update, &w->update, &tmp, RGN_OR);
    rgn_free(&tmp);
    if (erase)
        w->erase = 1;
}

/* Invalidate whatever shows through a screen rectangle (after a window moved or hid). */
static void invalidate_screen(IRect s, uint16_t first)
{
    for (uint16_t h = first; h; ) {
        Wnd *w = wnd_get(h);
        if (w->style & WS_VISIBLE) {
            IRect wr = wnd_screen_rect(w), x;
            if (rect_intersect(&x, &wr, &s)) {
                w->nc_dirty = 1;
                IRect cr = wnd_screen_client(w);
                if (rect_intersect(&x, &cr, &s)) {
                    rect_offset(&x, -cr.l, -cr.t);
                    wnd_invalidate(w, &x, 1);
                }
                invalidate_screen(s, w->child);
            }
        }
        h = w->next;
    }
}

static void expose_screen(IRect s)
{
    Wnd *d = wnd_get(g_desktop);
    wnd_invalidate(d, &s, 1);
    invalidate_screen(s, d->child);
    g_layout_gen++;
    video_dirty();
}

/* ---- positions ------------------------------------------------------------------------------- */

#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080

static void activate(uint16_t hwnd);

/* Recompute the client rectangle through WM_NCCALCSIZE. */
static void calc_client(Wnd *w)
{
    CPU *c = g_cpu;
    uint16_t sp = c->sp.x;
    uint32_t rp = stack_alloc(8);
    wr_rect(rp, w->wr);
    wnd_send(w->hwnd, WM_NCCALCSIZE, 0, rp);
    w = wnd_get(w->hwnd);
    if (w) {
        w->cr = rd_rect(rp);
        if (w->cr.r < w->cr.l) w->cr.r = w->cr.l;
        if (w->cr.b < w->cr.t) w->cr.b = w->cr.t;
    }
    c->sp.x = sp;
}

static void set_window_pos(uint16_t hwnd, uint16_t after, int x, int y, int cx, int cy, uint16_t flags)
{
    Wnd *w = wnd_get(hwnd);
    if (!w || hwnd == g_desktop)
        return;
    LOGD("SetWindowPos(%04x \"%s\", after %04x, %d,%d %dx%d, flags %04x)", hwnd, w->text, after, x, y, cx, cy, flags);
    CPU *c = g_cpu;
    uint16_t sp = c->sp.x;
    if (flags & SWP_NOMOVE) { x = w->wr.l; y = w->wr.t; }
    if (flags & SWP_NOSIZE) { cx = w->wr.r - w->wr.l; cy = w->wr.b - w->wr.t; }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    /* WM_WINDOWPOSCHANGING may adjust the request */
    uint32_t wp = stack_alloc(14);
    uint16_t s = FAR_SEL(wp), o = FAR_OFF(wp);
    wr16(s, o, hwnd); wr16(s, o + 2, after); wr16(s, o + 4, (uint16_t)x); wr16(s, o + 6, (uint16_t)y);
    wr16(s, o + 8, (uint16_t)cx); wr16(s, o + 10, (uint16_t)cy); wr16(s, o + 12, flags);
    if (!(flags & SWP_NOSIZE) || !(flags & SWP_NOMOVE) || (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)))
        wnd_send(hwnd, WM_WINDOWPOSCHANGING, 0, wp);
    x = (int16_t)rd16(s, o + 4); y = (int16_t)rd16(s, o + 6);
    cx = (int16_t)rd16(s, o + 8); cy = (int16_t)rd16(s, o + 10); flags = rd16(s, o + 12);
    w = wnd_get(hwnd);
    if (!w) {
        c->sp.x = sp;
        return;
    }
    IRect old = wnd_screen_rect(w);
    int was_visible = wnd_visible(w);
    IRect nr = irect(x, y, x + cx, y + cy);
    int moved = memcmp(&nr, &w->wr, sizeof(IRect)) != 0;
    w->wr = nr;
    if (moved || (flags & SWP_FRAMECHANGED))
        calc_client(w);
    w = wnd_get(hwnd);
    if (!w) {
        c->sp.x = sp;
        return;
    }
    if (!(flags & SWP_NOZORDER) && after != 0xFFFF && after != hwnd) {
        unlink_wnd(w);
        link_wnd(w, after == 0xFFFE ? 0 : after);
    }
    if (flags & SWP_SHOWWINDOW)
        w->style |= WS_VISIBLE;
    if (flags & SWP_HIDEWINDOW)
        w->style &= ~WS_VISIBLE;
    int now_visible = wnd_visible(w);
    if (was_visible)
        expose_screen(old);
    if (now_visible) {
        IRect nw = wnd_screen_rect(w);
        expose_screen(nw);
    }
    g_layout_gen++;
    wr16(s, o + 4, (uint16_t)x); wr16(s, o + 6, (uint16_t)y);
    wr16(s, o + 8, (uint16_t)cx); wr16(s, o + 10, (uint16_t)cy); wr16(s, o + 12, flags);
    wnd_send(hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    c->sp.x = sp;
    if ((flags & SWP_HIDEWINDOW) && g_active == hwnd)
        activate(0);
    if ((flags & SWP_SHOWWINDOW) && !(flags & SWP_NOACTIVATE) && !(w->style & WS_CHILD))
        activate(hwnd);
}

/* ---- activation and focus --------------------------------------------------------------- */

uint16_t user_set_focus(uint16_t hwnd)
{
    uint16_t old = g_focus;
    if (old == hwnd)
        return old;
    if (hwnd && !wnd_get(hwnd))
        return 0;
    g_focus = hwnd;
    if (old && wnd_get(old))
        wnd_send(old, WM_KILLFOCUS, hwnd, 0);
    if (hwnd && g_focus == hwnd)
        wnd_send(hwnd, WM_SETFOCUS, old, 0);
    return old;
}

static void activate(uint16_t hwnd)
{
    uint16_t old = g_active;
    if (old == hwnd)
        return;
    if (old && wnd_get(old)) {
        wnd_send(old, WM_NCACTIVATE, 0, 0);
        wnd_send(old, WM_ACTIVATE, 0, hwnd);
    }
    g_active = hwnd;
    if (!hwnd) {
        user_set_focus(0);
        return;
    }
    if (!old) {                                    /* the app becomes active */
        Wnd *d = wnd_get(g_desktop);
        for (uint16_t h = d->child; h; ) {
            Wnd *w = wnd_get(h);
            uint16_t next = w->next;
            wnd_send(h, WM_ACTIVATEAPP, 1, 0);
            h = next;
        }
    }
    if (!wnd_get(hwnd))
        return;
    wnd_send(hwnd, WM_NCACTIVATE, 1, 0);
    Wnd *w = wnd_get(hwnd);
    wnd_send(hwnd, WM_ACTIVATE, 1, old | (uint32_t)(w && (w->style & WS_MINIMIZE) ? 1 : 0) << 16);
    if (wnd_get(hwnd))
        wnd_send(hwnd, WM_QUERYNEWPALETTE, 0, 0);
}

/* ---- show, create, destroy -------------------------------------------------------------- */

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9

static int show_window(uint16_t hwnd, int cmd)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    LOGD("ShowWindow(%04x \"%s\", %d)", hwnd, w->text, cmd);
    int was = (w->style & WS_VISIBLE) != 0;
    int show = cmd != SW_HIDE;
    if (show == was && cmd != SW_SHOWMAXIMIZED && cmd != SW_RESTORE && cmd != SW_MINIMIZE &&
        cmd != SW_SHOWMINIMIZED && cmd != SW_SHOWMINNOACTIVE)
        return was;
    wnd_send(hwnd, WM_SHOWWINDOW, (uint16_t)show, 0);
    w = wnd_get(hwnd);
    if (!w)
        return was;
    uint16_t flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER;
    flags |= show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    if (cmd == SW_SHOWNOACTIVATE || cmd == SW_SHOWNA || cmd == SW_SHOWMINNOACTIVE || cmd == SW_MINIMIZE)
        flags |= SWP_NOACTIVATE;
    int x = w->wr.l, y = w->wr.t, cx = w->wr.r - w->wr.l, cy = w->wr.b - w->wr.t;
    if (cmd == SW_SHOWMAXIMIZED && !(w->style & WS_CHILD)) {
        IRect in = nc_insets(w->style | WS_MAXIMIZE, w->exstyle, w->hmenu != 0);
        w->style = (w->style | WS_MAXIMIZE) & ~WS_MINIMIZE;
        x = -in.l; y = -in.t; cx = SCREEN_W + in.l + in.r; cy = SCREEN_H + in.t + in.b;
        flags &= (uint16_t)~(SWP_NOSIZE | SWP_NOMOVE);
        flags |= SWP_FRAMECHANGED;
    } else if (cmd == SW_RESTORE || cmd == SW_SHOWNORMAL) {
        if (w->style & (WS_MINIMIZE | WS_MAXIMIZE))
            flags |= SWP_FRAMECHANGED;
        w->style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
    } else if (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE) {
        w->style = (w->style | WS_MINIMIZE) & ~WS_MAXIMIZE;
        flags |= SWP_FRAMECHANGED;
    }
    set_window_pos(hwnd, 0, x, y, cx, cy, flags);
    w = wnd_get(hwnd);
    if (w && (flags & SWP_FRAMECHANGED))
        wnd_send(hwnd, WM_SIZE, (w->style & WS_MAXIMIZE) ? 2 : (w->style & WS_MINIMIZE) ? 1 : 0,
                 (uint32_t)(uint16_t)(w->cr.r - w->cr.l) | (uint32_t)(uint16_t)(w->cr.b - w->cr.t) << 16);
    if (!show && g_focus && (g_focus == hwnd || is_child_of(hwnd, g_focus)))
        user_set_focus(0);
    return was;
}

static void free_wnd(Wnd *w)
{
    if (w->owndc)
        gdi_free_window_dc(w->owndc);
    rgn_free(&w->update);
    free(w->text);
    free(w->extra);
    h_free(w->hwnd);
    free(w);
}

static void destroy_tree(uint16_t hwnd)
{
    Wnd *w = wnd_get(hwnd);
    if (!w || w->destroying)
        return;
    w->destroying = 1;
    wnd_send(hwnd, WM_DESTROY, 0, 0);
    while ((w = wnd_get(hwnd)) && w->child)
        destroy_tree(w->child);
    wnd_send(hwnd, WM_NCDESTROY, 0, 0);
    w = wnd_get(hwnd);
    if (!w)
        return;
    user_kill_window_timers(hwnd);
    unlink_wnd(w);
    if (g_focus == hwnd) g_focus = 0;
    if (g_capture == hwnd) g_capture = 0;
    if (g_active == hwnd) g_active = 0;
    free_wnd(w);
}

static int destroy_window(uint16_t hwnd)
{
    Wnd *w = wnd_get(hwnd);
    if (!w || hwnd == g_desktop)
        return 0;
    int top = !(w->style & WS_CHILD);
    uint16_t parent = w->parent;
    if (w->style & WS_VISIBLE)
        set_window_pos(hwnd, 0, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    w = wnd_get(hwnd);
    if (w && (w->style & WS_CHILD) && parent && !(w->exstyle & 0x0004 /* WS_EX_NOPARENTNOTIFY */))
        wnd_send(parent, WM_PARENTNOTIFY, WM_DESTROY, hwnd | (uint32_t)w->id << 16);
    int was_active = g_active == hwnd;
    destroy_tree(hwnd);
    if (top && was_active) {
        g_active = 0;
        Wnd *d = wnd_get(g_desktop);
        for (uint16_t h = d->child; h; h = wnd_get(h)->next)
            if (wnd_get(h)->style & WS_VISIBLE) {
                activate(h);
                break;
            }
    }
    g_layout_gen++;
    return 1;
}

#define CW_USEDEFAULT ((int16_t)0x8000)

static uint16_t create_window(uint32_t ex, uint32_t cls_far, uint32_t title_far, uint32_t style, int x, int y, int cx,
                              int cy, uint16_t parent, uint16_t menu, uint16_t hinst, uint32_t param)
{
    WndClass *k = class_find(cls_far, NULL);
    if (!k) {
        LOGW("CreateWindow: unknown class %s", FAR_SEL(cls_far) ? guest_ptr(cls_far) : "(atom)");
        return 0;
    }
    if ((style & WS_CHILD) && !wnd_get(parent))
        return 0;
    Wnd *w = calloc(1, sizeof(Wnd));
    w->hwnd = h_alloc(HT_WND, w);
    w->cls = k;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = ex;
    w->wndproc = k->wndproc;
    w->hinst = hinst;
    if (style & WS_CHILD) {
        w->parent = parent;
        w->id = menu;
    } else {
        w->owner = parent;
        w->hmenu = menu;
        if ((style & WS_CAPTION) == WS_CAPTION || !(style & WS_POPUP))
            w->style |= WS_CLIPSIBLINGS;
    }
    w->extra = calloc(1, (size_t)(k->wnd_extra > 0 ? k->wnd_extra : 0) + 4);
    w->text = calloc(1, 1);
    rgn_init(&w->update);
    if (!(style & (WS_CHILD | WS_POPUP))) {        /* overlapped: defaults */
        if (x == CW_USEDEFAULT) { x = 0; y = 0; }
        if (cx == CW_USEDEFAULT) { cx = SCREEN_W * 3 / 4; cy = SCREEN_H * 3 / 4; }
    } else {
        if (x == CW_USEDEFAULT) x = y = 0;
        if (cx == CW_USEDEFAULT) cx = cy = 0;
    }
    w->wr = irect(x, y, x + cx, y + cy);
    IRect in = nc_insets(w->style, ex, w->hmenu != 0);
    w->cr = irect(w->wr.l + in.l, w->wr.t + in.t, w->wr.r - in.r, w->wr.b - in.b);
    link_wnd(w, 0);
    uint16_t hwnd = w->hwnd;

    CPU *c = g_cpu;
    uint16_t sp = c->sp.x;
    uint32_t cs = stack_alloc(34);
    uint16_t s = FAR_SEL(cs), o = FAR_OFF(cs);
    wr32(s, o, param); wr16(s, o + 4, hinst); wr16(s, o + 6, menu); wr16(s, o + 8, parent);
    wr16(s, o + 10, (uint16_t)cy); wr16(s, o + 12, (uint16_t)cx); wr16(s, o + 14, (uint16_t)y); wr16(s, o + 16, (uint16_t)x);
    wr32(s, o + 18, style); wr32(s, o + 22, title_far); wr32(s, o + 26, cls_far); wr32(s, o + 30, ex);
    LOGD("CreateWindow(%s, \"%s\", %08x, %d,%d %dx%d, parent %04x) = %04x", k->name,
         title_far && FAR_SEL(title_far) ? guest_ptr(title_far) : "", style, x, y, cx, cy, parent, hwnd);
    if (!wnd_send(hwnd, WM_NCCREATE, 0, cs)) {
        LOGW("WM_NCCREATE failed for %s", k->name);
        c->sp.x = sp;
        destroy_tree(hwnd);
        return 0;
    }
    if (!wnd_get(hwnd)) {
        c->sp.x = sp;
        return 0;
    }
    calc_client(wnd_get(hwnd));
    if ((int32_t)wnd_send(hwnd, WM_CREATE, 0, cs) == -1) {
        LOGW("WM_CREATE failed for %s", k->name);
        c->sp.x = sp;
        destroy_tree(hwnd);
        return 0;
    }
    c->sp.x = sp;
    w = wnd_get(hwnd);
    if (!w)
        return 0;
    wnd_send(hwnd, WM_SIZE, 0, (uint32_t)(uint16_t)(w->cr.r - w->cr.l) | (uint32_t)(uint16_t)(w->cr.b - w->cr.t) << 16);
    w = wnd_get(hwnd);
    if (!w)
        return 0;
    wnd_send(hwnd, WM_MOVE, 0, (uint32_t)(uint16_t)w->cr.l | (uint32_t)(uint16_t)w->cr.t << 16);
    w = wnd_get(hwnd);
    if (w && (w->style & WS_CHILD) && !(ex & 0x0004))
        wnd_send(parent, WM_PARENTNOTIFY, WM_CREATE, hwnd | (uint32_t)w->id << 16);
    if ((style & WS_VISIBLE) && wnd_get(hwnd)) {
        int cmd = (style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED : (style & WS_MINIMIZE) ? SW_SHOWMINIMIZED : SW_SHOW;
        show_window(hwnd, cmd);
    }
    return wnd_get(hwnd) ? hwnd : 0;
}

/* ---- painting ------------------------------------------------------------------------------ */

uint16_t user_begin_paint(uint16_t hwnd, int *erase_needed, IRect *paint)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    if (w->nc_dirty) {
        w->nc_dirty = 0;
        wnd_send(hwnd, WM_NCPAINT, 1, 0);
        w = wnd_get(hwnd);
        if (!w)
            return 0;
    }
    uint16_t hdc = gdi_screen_dc(hwnd, 0);
    DC *dc = dc_get(hdc);
    IRect cr = wnd_screen_client(w);
    rgn_copy(&dc->paint_rgn, &w->update);
    rgn_offset(&dc->paint_rgn, cr.l, cr.t);
    dc->paint = 1;
    *paint = w->update.box;
    int erase = w->erase;
    rgn_set_rect(&w->update, irect(0, 0, 0, 0));
    w->erase = 0;
    *erase_needed = 0;
    if (erase)
        *erase_needed = !wnd_send(hwnd, WM_ERASEBKGND, hdc, 0);
    return hdc;
}

static void BeginPaint(CPU *c)         /* HDC BeginPaint(HWND, PAINTSTRUCT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    int erase;
    IRect paint;
    uint16_t hdc = user_begin_paint((uint16_t)a[0], &erase, &paint);
    uint8_t *ps = (uint8_t *)guest_ptr(a[1]);
    memset(ps, 0, 32);
    wr16(FAR_SEL(a[1]), FAR_OFF(a[1]), hdc);
    wr16(FAR_SEL(a[1]), FAR_OFF(a[1]) + 2, (uint16_t)erase);
    wr_rect(a[1] + 4, paint);
    c->a.x = hdc;
    api_ret(c, n);
}

static void EndPaint(CPU *c)           /* void EndPaint(HWND, const PAINTSTRUCT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    gdi_release_dc(rd16(FAR_SEL(a[1]), FAR_OFF(a[1])));
    api_ret(c, n);
}

static void InvalidateRect(CPU *c)     /* void InvalidateRect(HWND, const RECT FAR *, BOOL erase) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    if (!a[0]) {
        expose_screen(a[1] ? rd_rect(a[1]) : irect(0, 0, SCREEN_W, SCREEN_H));
    } else if (w) {
        IRect r;
        if (a[1])
            r = rd_rect(a[1]);
        wnd_invalidate(w, a[1] ? &r : NULL, (int)a[2]);
    }
    api_ret(c, n);
}

static void InvalidateRgn(CPU *c)      /* void InvalidateRgn(HWND, HRGN, BOOL) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    Region *g = h_get((uint16_t)a[1], HT_RGN);
    if (w) {
        if (g) {
            Region clip;
            rgn_init(&clip);
            IRect cr = wnd_screen_client(w);
            rgn_set_rect(&clip, irect(0, 0, cr.r - cr.l, cr.b - cr.t));
            rgn_combine(&clip, &clip, g, RGN_AND);
            rgn_combine(&w->update, &w->update, &clip, RGN_OR);
            rgn_free(&clip);
            if (a[2])
                w->erase = 1;
        } else {
            wnd_invalidate(w, NULL, (int)a[2]);
        }
    }
    api_ret(c, n);
}

static void ValidateRect(CPU *c)       /* void ValidateRect(HWND, const RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    if (w) {
        if (a[1]) {
            Region tmp;
            rgn_init(&tmp);
            rgn_set_rect(&tmp, rd_rect(a[1]));
            rgn_combine(&w->update, &w->update, &tmp, RGN_DIFF);
            rgn_free(&tmp);
        } else {
            rgn_set_rect(&w->update, irect(0, 0, 0, 0));
        }
    }
    api_ret(c, n);
}

static void ValidateRgn(CPU *c)        /* void ValidateRgn(HWND, HRGN) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    Region *g = h_get((uint16_t)a[1], HT_RGN);
    if (w) {
        if (g)
            rgn_combine(&w->update, &w->update, g, RGN_DIFF);
        else
            rgn_set_rect(&w->update, irect(0, 0, 0, 0));
    }
    api_ret(c, n);
}

static void GetUpdateRect(CPU *c)      /* BOOL GetUpdateRect(HWND, RECT FAR *, BOOL erase) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    IRect r = w ? w->update.box : irect(0, 0, 0, 0);
    if (a[1])
        wr_rect(a[1], r);
    c->a.x = w && w->update.n;
    api_ret(c, n);
}

static void GetUpdateRgn(CPU *c)       /* int GetUpdateRgn(HWND, HRGN, BOOL erase) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    Region *g = h_get((uint16_t)a[1], HT_RGN);
    c->a.x = 0;
    if (w && g) {
        rgn_copy(g, &w->update);
        c->a.x = (uint16_t)rgn_kind(g);
    }
    api_ret(c, n);
}

static void UpdateWindow(CPU *c)       /* void UpdateWindow(HWND) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    if (w && wnd_visible(w)) {
        if (w->nc_dirty) {
            w->nc_dirty = 0;
            wnd_send(w->hwnd, WM_NCPAINT, 1, 0);
        }
        w = wnd_get((uint16_t)a[0]);
        if (w && w->update.n)
            wnd_send(w->hwnd, WM_PAINT, 0, 0);
    }
    api_ret(c, n);
}

/* ---- DefWindowProc --------------------------------------------------------------------------- */

static void draw_frame(Wnd *w)
{
    uint16_t hdc = gdi_screen_dc(w->hwnd, 1);
    DC *dc = dc_get(hdc);
    IRect wr = wnd_screen_rect(w), r = irect(0, 0, wr.r - wr.l, wr.b - wr.t);
    IRect in = nc_insets(w->style, w->exstyle, w->hmenu != 0);
    uint16_t black = gdi_stock(4 /* BLACK_BRUSH */);
    if (in.l || in.t || in.r || in.b) {
        uint16_t frame = gdi_solid_brush(0xC0C0C0);
        IRect cr = wnd_screen_client(w);
        rect_offset(&cr, -wr.l, -wr.t);
        /* everything outside the client area: frame colour, black outline */
        gdi_fill_rect(dc, irect(r.l, r.t, r.r, cr.t), frame);
        gdi_fill_rect(dc, irect(r.l, cr.b, r.r, r.b), frame);
        gdi_fill_rect(dc, irect(r.l, cr.t, cr.l, cr.b), frame);
        gdi_fill_rect(dc, irect(cr.r, cr.t, r.r, cr.b), frame);
        gdi_frame_rect(dc, r, black);
        if ((w->style & WS_CAPTION) == WS_CAPTION) {
            int f = in.l;
            IRect cap = irect(f, f, r.r - f, f + 20 - 1);
            int active = g_active == w->hwnd;
            gdi_fill_rect(dc, cap, gdi_solid_brush(active ? 0x800000 : 0xFFFFFF));    /* navy / white */
            gdi_fill_rect(dc, irect(cap.l, cap.b - 1, cap.r, cap.b), black);
            IRect text = irect(cap.l, cap.t, cap.r, cap.b - 1);
            if (w->style & WS_SYSMENU) {               /* the system menu box: a bar on grey */
                IRect box = irect(cap.l, cap.t, cap.l + 20 - 1, cap.b - 1);
                gdi_fill_rect(dc, box, gdi_solid_brush(0xC0C0C0));
                gdi_fill_rect(dc, irect(box.r, box.t, box.r + 1, box.b), black);
                int my = (box.t + box.b) / 2;
                gdi_fill_rect(dc, irect(box.l + 3, my - 1, box.r - 3, my + 2), gdi_solid_brush(0xFFFFFF));
                gdi_frame_rect(dc, irect(box.l + 2, my - 2, box.r - 2, my + 3), black);
                text.l = box.r + 1;
            }
            if (w->text && *w->text) {
                DC saved = *dc;
                dc->hfont = gdi_stock(13);             /* SYSTEM_FONT */
                dc->bk_mode = 1;                       /* TRANSPARENT */
                dc->text_color = active ? 0xFFFFFF : 0x000000;
                gdi_draw_text_box(dc, text, w->text, 1, 0, 1, 1);
                dc->hfont = saved.hfont;
                dc->bk_mode = saved.bk_mode;
                dc->text_color = saved.text_color;
            }
        }
    }
    gdi_release_dc(hdc);
}

static uint16_t hit_test(Wnd *w, int x, int y)
{
    IRect wr = wnd_screen_rect(w), cr = wnd_screen_client(w);
    if (!rect_has_pt(&wr, x, y))
        return 0;                                  /* HTNOWHERE */
    if (rect_has_pt(&cr, x, y))
        return HTCLIENT;
    if ((w->style & WS_CAPTION) == WS_CAPTION && y < cr.t)
        return HTCAPTION;
    return 18;                                     /* HTBORDER */
}

static uint16_t g_arrow;                           /* cursor for non-client areas */

uint32_t def_window_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    switch (msg) {
    case WM_NCCREATE: {
        uint32_t title = rd32(FAR_SEL(lp), FAR_OFF(lp) + 22);
        if (title && FAR_SEL(title)) {
            free(w->text);
            w->text = strdup(guest_ptr(title));
        }
        return 1;
    }
    case WM_NCCALCSIZE: {
        IRect r = rd_rect(lp);
        IRect in = nc_insets(w->style, w->exstyle, w->hmenu != 0);
        r.l += in.l; r.t += in.t; r.r -= in.r; r.b -= in.b;
        wr_rect(lp, r);
        return 0;
    }
    case WM_NCHITTEST:
        return hit_test(w, (int16_t)(lp & 0xFFFF), (int16_t)(lp >> 16));
    case WM_NCPAINT:
        draw_frame(w);
        return 0;
    case WM_NCACTIVATE:
        if ((w->style & WS_CAPTION) == WS_CAPTION && wnd_visible(w))
            draw_frame(w);
        return 1;
    case WM_PAINT: {
        int erase;
        IRect pr;
        uint16_t hdc = user_begin_paint(hwnd, &erase, &pr);
        gdi_release_dc(hdc);
        return 0;
    }
    case WM_ERASEBKGND: {
        uint16_t br = w->cls->hbrush;
        if (!br)
            return 0;
        if (br <= 21)                              /* COLOR_xxx + 1 */
            br = gdi_solid_brush(user_sys_color(br - 1));
        DC *dc = dc_get(wp);
        if (!dc)
            return 0;
        IRect cr = wnd_screen_client(w);
        gdi_fill_rect(dc, irect(0, 0, cr.r - cr.l, cr.b - cr.t), br);
        return 1;
    }
    case WM_SETCURSOR:
        if (w->parent && wnd_send(w->parent, WM_SETCURSOR, wp, lp))
            return 1;
        if ((lp & 0xFFFF) == HTCLIENT) {
            if (w->cls->hcursor)
                user_set_cursor(w->cls->hcursor);
            return 0;
        }
        user_set_cursor(g_arrow);
        return 0;
    case WM_MOUSEACTIVATE:
        if (w->parent) {
            uint32_t r = wnd_send(w->parent, WM_MOUSEACTIVATE, wp, lp);
            if (r)
                return r;
        }
        return 1;                                  /* MA_ACTIVATE */
    case WM_CLOSE:
        destroy_window(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
    case 0x0013:                                   /* WM_QUERYOPEN */
        return 1;
    case WM_WINDOWPOSCHANGED: {
        uint16_t flags = rd16(FAR_SEL(lp), FAR_OFF(lp) + 12);
        if (!(flags & SWP_NOMOVE))
            wnd_send(hwnd, WM_MOVE, 0, (uint32_t)(uint16_t)w->cr.l | (uint32_t)(uint16_t)w->cr.t << 16);
        w = wnd_get(hwnd);
        if (w && !(flags & SWP_NOSIZE))
            wnd_send(hwnd, WM_SIZE, (w->style & WS_MAXIMIZE) ? 2 : (w->style & WS_MINIMIZE) ? 1 : 0,
                     (uint32_t)(uint16_t)(w->cr.r - w->cr.l) | (uint32_t)(uint16_t)(w->cr.b - w->cr.t) << 16);
        return 0;
    }
    case WM_ACTIVATE:
        if (wp && !(w->style & WS_MINIMIZE))
            user_set_focus(hwnd);
        return 0;
    case WM_SETTEXT:
        free(w->text);
        w->text = strdup(lp ? guest_ptr(lp) : "");
        if ((w->style & WS_CAPTION) == WS_CAPTION && wnd_visible(w))
            draw_frame(w);
        return 1;
    case WM_GETTEXT: {
        size_t len = strlen(w->text);
        if (!wp)
            return 0;
        if (len > (size_t)wp - 1)
            len = (size_t)wp - 1;
        memcpy(guest_ptr(lp), w->text, len);
        guest_ptr(lp)[len] = 0;
        return (uint32_t)len;
    }
    case WM_GETTEXTLENGTH:
        return (uint32_t)strlen(w->text);
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case 0xF060: wnd_send(hwnd, WM_CLOSE, 0, 0); break;      /* SC_CLOSE */
        case 0xF020: show_window(hwnd, SW_MINIMIZE); break;
        case 0xF030: show_window(hwnd, SW_SHOWMAXIMIZED); break;
        case 0xF120: show_window(hwnd, SW_RESTORE); break;
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == 0x73 && (lp & (1u << 29)))      /* Alt+F4 */
            wnd_send(hwnd, WM_SYSCOMMAND, 0xF060, 0);
        return 0;
    case WM_CANCELMODE:
        if (g_capture == hwnd)
            g_capture = 0;
        return 0;
    case 0x0019:                                   /* WM_CTLCOLOR */
        return gdi_stock(0 /* WHITE_BRUSH */);
    default:
        return 0;
    }
}

static void DefWindowProc(CPU *c)      /* LRESULT DefWindowProc(HWND, UINT, WPARAM, LPARAM) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    api_ret32(c, n, def_window_proc((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]));
}

/* LRESULT CallWindowProc(WNDPROC, HWND, UINT, WPARAM, LPARAM) */
static void CallWindowProc(CPU *c)
{
    uint32_t a[5];
    uint16_t n = api_args(c, "lwwwl", a);
    uint16_t sp = c->sp.x;
    /* the arguments stay where they are while the callee runs below them */
    uint32_t r = wnd_call_proc(a[0], (uint16_t)a[1], (uint16_t)a[2], (uint16_t)a[3], a[4]);
    c->sp.x = sp;
    api_ret32(c, n, r);
}

/* Built-in class procedures: DefWindowProc behaviour for now (controls draw nothing). */
static void DefaultControlProc(CPU *c) { DefWindowProc(c); }

/* ---- classes API -------------------------------------------------------------------------- */

static void RegisterClass(CPU *c)      /* ATOM RegisterClass(const WNDCLASS FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    uint32_t name_far = rd32(s, o + 22);
    const char *name = FAR_SEL(name_far) ? guest_ptr(name_far) : "";
    c->a.x = 0;
    if (FAR_SEL(name_far) && !class_find(0, name)) {
        WndClass *k = class_add(name, rd16(s, o), rd32(s, o + 2), (int16_t)rd16(s, o + 6), (int16_t)rd16(s, o + 8),
                                rd16(s, o + 10), rd16(s, o + 14), rd16(s, o + 16));
        if (k) {
            k->hicon = rd16(s, o + 12);
            uint32_t menu = rd32(s, o + 18);
            if (menu && FAR_SEL(menu))
                snprintf(k->menu_name, sizeof(k->menu_name), "%s", guest_ptr(menu));
            c->a.x = k->atom;
            LOGD("RegisterClass(%s, proc %04x:%04x) = %04x", name, FAR_SEL(k->wndproc), FAR_OFF(k->wndproc), k->atom);
        }
    }
    api_ret(c, n);
}

static void UnregisterClass(CPU *c)    /* BOOL UnregisterClass(LPCSTR, HINSTANCE) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    WndClass *k = class_find(a[0], NULL);
    c->a.x = 0;
    for (int i = 0; k && !k->builtin && i < MAX_CLASSES; i++)
        if (g_class[i] == k) {
            free(k->extra);
            free(k);
            g_class[i] = NULL;
            c->a.x = 1;
        }
    api_ret(c, n);
}

static void GetClassInfo(CPU *c)       /* BOOL GetClassInfo(HINSTANCE, LPCSTR, WNDCLASS FAR *) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wll", a);
    WndClass *k = class_find(a[1], NULL);
    c->a.x = 0;
    if (k) {
        uint16_t s = FAR_SEL(a[2]), o = FAR_OFF(a[2]);
        wr16(s, o, k->style); wr32(s, o + 2, k->wndproc); wr16(s, o + 6, (uint16_t)k->cls_extra);
        wr16(s, o + 8, (uint16_t)k->wnd_extra); wr16(s, o + 10, k->hinst); wr16(s, o + 12, k->hicon);
        wr16(s, o + 14, k->hcursor); wr16(s, o + 16, k->hbrush); wr32(s, o + 18, 0); wr32(s, o + 22, a[1]);
        c->a.x = 1;
    }
    api_ret(c, n);
}

static void GetClassName(CPU *c)       /* int GetClassName(HWND, LPSTR, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = 0;
    if (w && a[2]) {
        size_t len = strlen(w->cls->name);
        if (len > a[2] - 1)
            len = a[2] - 1;
        memcpy(guest_ptr(a[1]), w->cls->name, len);
        guest_ptr(a[1])[len] = 0;
        c->a.x = (uint16_t)len;
    }
    api_ret(c, n);
}

static uint32_t class_field(WndClass *k, int16_t idx, int set, uint32_t v, int size)
{
    uint32_t old = 0;
    if (idx >= 0) {
        if (idx + size <= k->cls_extra) {
            old = size == 4 ? rd32_host(k->extra + idx) : rd16_host(k->extra + idx);
            if (set) {
                if (size == 4) wr32_host(k->extra + idx, v); else wr16_host(k->extra + idx, (uint16_t)v);
            }
        }
        return old;
    }
    switch (idx) {
    case -10: old = k->hbrush; if (set) k->hbrush = (uint16_t)v; break;     /* GCW_HBRBACKGROUND */
    case -12: old = k->hcursor; if (set) k->hcursor = (uint16_t)v; break;   /* GCW_HCURSOR */
    case -14: old = k->hicon; if (set) k->hicon = (uint16_t)v; break;       /* GCW_HICON */
    case -16: old = k->hinst; break;                                         /* GCW_HMODULE */
    case -18: old = (uint16_t)k->wnd_extra; break;                           /* GCW_CBWNDEXTRA */
    case -20: old = (uint16_t)k->cls_extra; break;                           /* GCW_CBCLSEXTRA */
    case -24: old = k->wndproc; if (set) k->wndproc = v; break;              /* GCL_WNDPROC */
    case -26: old = k->style; if (set) k->style = (uint16_t)v; break;        /* GCW_STYLE */
    case -32: old = k->atom; break;                                          /* GCW_ATOM */
    }
    return old;
}

static void GetClassWord(CPU *c)       /* WORD GetClassWord(HWND, int) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w ? (uint16_t)class_field(w->cls, (int16_t)a[1], 0, 0, 2) : 0;
    api_ret(c, n);
}

static void SetClassWord(CPU *c)       /* WORD SetClassWord(HWND, int, WORD) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w ? (uint16_t)class_field(w->cls, (int16_t)a[1], 1, a[2], 2) : 0;
    api_ret(c, n);
}

static void GetClassLong(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    api_ret32(c, n, w ? class_field(w->cls, (int16_t)a[1], 0, 0, 4) : 0);
}

static void SetClassLong(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    api_ret32(c, n, w ? class_field(w->cls, (int16_t)a[1], 1, a[2], 4) : 0);
}

/* ---- window API -------------------------------------------------------------------------- */

/* HWND CreateWindow(LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int cx, int cy,
 *                   HWND parent, HMENU, HINSTANCE, void FAR *param) */
static void CreateWindow(CPU *c)
{
    uint32_t a[11];
    uint16_t n = api_args(c, "lllwwwwwwwl", a);
    c->a.x = create_window(0, a[0], a[1], a[2], (int16_t)a[3], (int16_t)a[4], (int16_t)a[5], (int16_t)a[6],
                           (uint16_t)a[7], (uint16_t)a[8], (uint16_t)a[9], a[10]);
    api_ret(c, n);
}

static void CreateWindowEx(CPU *c)
{
    uint32_t a[12];
    uint16_t n = api_args(c, "llllwwwwwwwl", a);
    c->a.x = create_window(a[0], a[1], a[2], a[3], (int16_t)a[4], (int16_t)a[5], (int16_t)a[6], (int16_t)a[7],
                           (uint16_t)a[8], (uint16_t)a[9], (uint16_t)a[10], a[11]);
    api_ret(c, n);
}

static void DestroyWindow(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = (uint16_t)destroy_window((uint16_t)a[0]);
    api_ret(c, n);
}

static void ShowWindow(CPU *c)         /* BOOL ShowWindow(HWND, int) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = (uint16_t)show_window((uint16_t)a[0], (int16_t)a[1]);
    api_ret(c, n);
}

static void SetWindowPos(CPU *c)       /* void SetWindowPos(HWND, HWND after, int x, int y, int cx, int cy, UINT) */
{
    uint32_t a[7];
    uint16_t n = api_args(c, "wwwwwww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    uint16_t flags = (uint16_t)a[6];
    if (w) {
        uint16_t after = (uint16_t)a[1];
        if (after == 0xFFFF)                       /* HWND_TOPMOST */
            after = 0;
        else if (after == 0xFFFE)                  /* HWND_NOTOPMOST */
            after = 0;
        set_window_pos((uint16_t)a[0], after, (int16_t)a[2], (int16_t)a[3], (int16_t)a[4], (int16_t)a[5], flags);
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void MoveWindow(CPU *c)         /* void MoveWindow(HWND, int x, int y, int cx, int cy, BOOL repaint) */
{
    uint32_t a[6];
    uint16_t n = api_args(c, "wwwwww", a);
    set_window_pos((uint16_t)a[0], 0, (int16_t)a[1], (int16_t)a[2], (int16_t)a[3], (int16_t)a[4],
                   SWP_NOZORDER | SWP_NOACTIVATE | (a[5] ? 0 : SWP_NOREDRAW));
    c->a.x = 1;
    api_ret(c, n);
}

static void BringWindowToTop(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    set_window_pos((uint16_t)a[0], 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    c->a.x = 1;
    api_ret(c, n);
}

static void GetWindowRect(CPU *c)      /* void GetWindowRect(HWND, RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Wnd *w = wnd_get((uint16_t)a[0] ? (uint16_t)a[0] : g_desktop);
    wr_rect(a[1], w ? (w->hwnd == g_desktop ? irect(0, 0, SCREEN_W, SCREEN_H) : wnd_screen_rect(w)) : irect(0, 0, 0, 0));
    api_ret(c, n);
}

static void GetClientRect(CPU *c)      /* void GetClientRect(HWND, RECT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Wnd *w = wnd_get((uint16_t)a[0] ? (uint16_t)a[0] : g_desktop);
    IRect r = irect(0, 0, 0, 0);
    if (w) {
        IRect cr = wnd_screen_client(w);
        r = irect(0, 0, cr.r - cr.l, cr.b - cr.t);
    }
    wr_rect(a[1], r);
    api_ret(c, n);
}

static IRect client_origin(uint16_t hwnd)
{
    Wnd *w = wnd_get(hwnd);
    return w ? wnd_screen_client(w) : irect(0, 0, 0, 0);
}

static void ClientToScreen(CPU *c)     /* void ClientToScreen(HWND, POINT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    IRect o = client_origin((uint16_t)a[0]);
    uint16_t s = FAR_SEL(a[1]), off = FAR_OFF(a[1]);
    wr16(s, off, (uint16_t)((int16_t)rd16(s, off) + o.l));
    wr16(s, off + 2, (uint16_t)((int16_t)rd16(s, off + 2) + o.t));
    api_ret(c, n);
}

static void ScreenToClient(CPU *c)     /* void ScreenToClient(HWND, POINT FAR *) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    IRect o = client_origin((uint16_t)a[0]);
    uint16_t s = FAR_SEL(a[1]), off = FAR_OFF(a[1]);
    wr16(s, off, (uint16_t)((int16_t)rd16(s, off) - o.l));
    wr16(s, off + 2, (uint16_t)((int16_t)rd16(s, off + 2) - o.t));
    api_ret(c, n);
}

static void MapWindowPoints(CPU *c)    /* void MapWindowPoints(HWND from, HWND to, POINT FAR *, UINT) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwlw", a);
    IRect f = a[0] ? client_origin((uint16_t)a[0]) : irect(0, 0, 0, 0);
    IRect t = a[1] ? client_origin((uint16_t)a[1]) : irect(0, 0, 0, 0);
    uint16_t s = FAR_SEL(a[2]), off = FAR_OFF(a[2]);
    for (uint32_t i = 0; i < a[3]; i++, off += 4) {
        wr16(s, off, (uint16_t)((int16_t)rd16(s, off) + f.l - t.l));
        wr16(s, off + 2, (uint16_t)((int16_t)rd16(s, off + 2) + f.t - t.t));
    }
    api_ret(c, n);
}

static void GetParent(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w ? ((w->style & WS_CHILD) ? w->parent : w->owner) : 0;
    api_ret(c, n);
}

static void GetWindow(CPU *c)          /* HWND GetWindow(HWND, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    uint16_t r = 0;
    if (w) {
        Wnd *p = wnd_get(w->parent ? w->parent : g_desktop);
        switch (a[1]) {
        case 0: r = p ? p->child : 0; break;                    /* GW_HWNDFIRST */
        case 1: {                                               /* GW_HWNDLAST */
            for (uint16_t h = p ? p->child : 0; h; h = wnd_get(h)->next)
                r = h;
            break;
        }
        case 2: r = w->next; break;                             /* GW_HWNDNEXT */
        case 3: {                                               /* GW_HWNDPREV */
            for (uint16_t h = p ? p->child : 0; h && h != w->hwnd; h = wnd_get(h)->next)
                r = h;
            break;
        }
        case 4: r = w->owner; break;                            /* GW_OWNER */
        case 5: r = w->child; break;                            /* GW_CHILD */
        }
    }
    c->a.x = r;
    api_ret(c, n);
}

static void GetTopWindow(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0] ? (uint16_t)a[0] : g_desktop);
    c->a.x = w ? w->child : 0;
    api_ret(c, n);
}

static void GetDesktopWindow(CPU *c)
{
    c->a.x = g_desktop;
    api_ret(c, 0);
}

static void IsWindow(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = wnd_get((uint16_t)a[0]) != NULL;
    api_ret(c, n);
}

static void IsChild(CPU *c)            /* BOOL IsChild(HWND parent, HWND) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = (uint16_t)is_child_of((uint16_t)a[0], (uint16_t)a[1]);
    api_ret(c, n);
}

static void IsWindowVisible(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w && wnd_visible(w);
    api_ret(c, n);
}

static void IsWindowEnabled(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w && !(w->style & WS_DISABLED);
    api_ret(c, n);
}

static void EnableWindow(CPU *c)       /* BOOL EnableWindow(HWND, BOOL): previous disabled state */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = 0;
    if (w) {
        int was_disabled = (w->style & WS_DISABLED) != 0;
        c->a.x = (uint16_t)was_disabled;
        if (a[1] && was_disabled) {
            w->style &= ~WS_DISABLED;
            wnd_send(w->hwnd, WM_ENABLE, 1, 0);
        } else if (!a[1] && !was_disabled) {
            w->style |= WS_DISABLED;
            if (g_focus == w->hwnd || is_child_of(w->hwnd, g_focus))
                user_set_focus(0);
            wnd_send(w->hwnd, WM_ENABLE, 0, 0);
        }
    }
    api_ret(c, n);
}

static void IsIconic(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w && (w->style & WS_MINIMIZE);
    api_ret(c, n);
}

static void IsZoomed(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w && (w->style & WS_MAXIMIZE);
    api_ret(c, n);
}

static void WindowFromPoint(CPU *c)    /* HWND WindowFromPoint(POINT) — passed by value */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = wnd_from_point((int16_t)(a[0] & 0xFFFF), (int16_t)(a[0] >> 16));
    api_ret(c, n);
}

static void ChildWindowFromPoint(CPU *c)   /* HWND ChildWindowFromPoint(HWND, POINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = 0;
    if (w) {
        IRect cr = wnd_screen_client(w);
        int x = (int16_t)(a[1] & 0xFFFF) + cr.l, y = (int16_t)(a[1] >> 16) + cr.t;
        c->a.x = (uint16_t)a[0];
        for (uint16_t h = w->child; h; h = wnd_get(h)->next) {
            IRect r = wnd_screen_rect(wnd_get(h));
            if ((wnd_get(h)->style & WS_VISIBLE) && rect_has_pt(&r, x, y)) {
                c->a.x = h;
                break;
            }
        }
    }
    api_ret(c, n);
}

static uint32_t window_field(Wnd *w, int16_t idx, int set, uint32_t v, int size)
{
    uint32_t old = 0;
    if (idx >= 0) {
        if (idx + size <= w->cls->wnd_extra) {
            old = size == 4 ? rd32_host(w->extra + idx) : rd16_host(w->extra + idx);
            if (set) {
                if (size == 4) wr32_host(w->extra + idx, v); else wr16_host(w->extra + idx, (uint16_t)v);
            }
        } else {
            LOGW("window extra bytes offset %d out of range (%d)", idx, w->cls->wnd_extra);
        }
        return old;
    }
    switch (idx) {
    case -4: old = w->wndproc; if (set) w->wndproc = v; break;              /* GWL_WNDPROC */
    case -6: old = w->hinst; if (set) w->hinst = (uint16_t)v; break;        /* GWW_HINSTANCE */
    case -8: old = (w->style & WS_CHILD) ? w->parent : w->owner;            /* GWW_HWNDPARENT */
        if (set && !(w->style & WS_CHILD)) w->owner = (uint16_t)v;
        break;
    case -12: old = (w->style & WS_CHILD) ? w->id : w->hmenu;               /* GWW_ID */
        if (set) { if (w->style & WS_CHILD) w->id = (uint16_t)v; else w->hmenu = (uint16_t)v; }
        break;
    case -16:                                                               /* GWL_STYLE */
        old = w->style;
        if (set) {
            uint32_t vis = w->style & WS_VISIBLE;
            w->style = (v & ~WS_VISIBLE) | vis;
            if ((v & WS_VISIBLE) != vis)
                show_window(w->hwnd, (v & WS_VISIBLE) ? SW_SHOWNA : SW_HIDE);
            g_layout_gen++;
        }
        break;
    case -20: old = w->exstyle; if (set) w->exstyle = v; break;             /* GWL_EXSTYLE */
    }
    return old;
}

static void GetWindowLong(CPU *c)      /* LONG GetWindowLong(HWND, int) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    api_ret32(c, n, w ? window_field(w, (int16_t)a[1], 0, 0, 4) : 0);
}

static void SetWindowLong(CPU *c)      /* LONG SetWindowLong(HWND, int, LONG) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wwl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    api_ret32(c, n, w ? window_field(w, (int16_t)a[1], 1, a[2], 4) : 0);
}

static void GetWindowWord(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w ? (uint16_t)window_field(w, (int16_t)a[1], 0, 0, 2) : 0;
    api_ret(c, n);
}

static void SetWindowWord(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w ? (uint16_t)window_field(w, (int16_t)a[1], 1, a[2], 2) : 0;
    api_ret(c, n);
}

static void SetWindowText(CPU *c)      /* void SetWindowText(HWND, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    wnd_send((uint16_t)a[0], WM_SETTEXT, 0, a[1]);
    api_ret(c, n);
}

static void GetWindowText(CPU *c)      /* int GetWindowText(HWND, LPSTR, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    c->a.x = (uint16_t)wnd_send((uint16_t)a[0], WM_GETTEXT, (uint16_t)a[2], a[1]);
    api_ret(c, n);
}

static void GetWindowTextLength(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = (uint16_t)wnd_send((uint16_t)a[0], WM_GETTEXTLENGTH, 0, 0);
    api_ret(c, n);
}

static void GetDlgCtrlID(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = w ? w->id : 0;
    api_ret(c, n);
}

static void GetDlgItem(CPU *c)         /* HWND GetDlgItem(HWND, int) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = 0;
    for (uint16_t h = w ? w->child : 0; h; h = wnd_get(h)->next)
        if (wnd_get(h)->id == (uint16_t)a[1]) {
            c->a.x = h;
            break;
        }
    api_ret(c, n);
}

static void SetFocus(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = user_set_focus((uint16_t)a[0]);
    api_ret(c, n);
}

static void GetFocus(CPU *c)
{
    c->a.x = g_focus;
    api_ret(c, 0);
}

static void SetCapture(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = g_capture;
    g_capture = (uint16_t)a[0];
    api_ret(c, n);
}

static void ReleaseCapture(CPU *c)
{
    g_capture = 0;
    api_ret(c, 0);
}

static void GetCapture(CPU *c)
{
    c->a.x = g_capture;
    api_ret(c, 0);
}

static void GetActiveWindow(CPU *c)
{
    c->a.x = g_active;
    api_ret(c, 0);
}

static void SetActiveWindow(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t old = g_active;
    activate((uint16_t)a[0]);
    c->a.x = old;
    api_ret(c, n);
}

static void AdjustWindowRect(CPU *c)   /* void AdjustWindowRect(RECT FAR *, DWORD style, BOOL menu) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "llw", a);
    IRect r = rd_rect(a[0]), in = nc_insets(a[1], 0, (int)a[2]);
    wr_rect(a[0], irect(r.l - in.l, r.t - in.t, r.r + in.r, r.b + in.b));
    api_ret(c, n);
}

static void AdjustWindowRectEx(CPU *c) /* void AdjustWindowRectEx(RECT FAR *, DWORD, BOOL, DWORD ex) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llwl", a);
    IRect r = rd_rect(a[0]), in = nc_insets(a[1], a[3], (int)a[2]);
    wr_rect(a[0], irect(r.l - in.l, r.t - in.t, r.r + in.r, r.b + in.b));
    api_ret(c, n);
}

static void FlashWindow(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = g_active == (uint16_t)a[0];
    api_ret(c, n);
}

static void RepaintScreen(CPU *c)
{
    expose_screen(irect(0, 0, SCREEN_W, SCREEN_H));
    api_ret(c, 0);
}

/* ---- properties ---------------------------------------------------------------------------- */

static int prop_find(Wnd *w, uint32_t name)
{
    for (int i = 0; i < w->nprops; i++) {
        if (FAR_SEL(name) == 0 ? w->props[i].atom == FAR_OFF(name)
                               : !w->props[i].atom && ieq(w->props[i].name, guest_ptr(name)))
            return i;
    }
    return -1;
}

static void SetProp(CPU *c)            /* BOOL SetProp(HWND, LPCSTR, HANDLE) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    c->a.x = 0;
    if (w) {
        int i = prop_find(w, a[1]);
        if (i < 0 && w->nprops < 8) {
            i = w->nprops++;
            memset(&w->props[i], 0, sizeof(w->props[i]));
            if (FAR_SEL(a[1]) == 0)
                w->props[i].atom = FAR_OFF(a[1]);
            else
                snprintf(w->props[i].name, sizeof(w->props[i].name), "%s", guest_ptr(a[1]));
        }
        if (i >= 0) {
            w->props[i].value = (uint16_t)a[2];
            c->a.x = 1;
        }
    }
    api_ret(c, n);
}

static void GetProp(CPU *c)            /* HANDLE GetProp(HWND, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    int i = w ? prop_find(w, a[1]) : -1;
    c->a.x = i >= 0 ? w->props[i].value : 0;
    api_ret(c, n);
}

static void RemoveProp(CPU *c)         /* HANDLE RemoveProp(HWND, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    Wnd *w = wnd_get((uint16_t)a[0]);
    int i = w ? prop_find(w, a[1]) : -1;
    c->a.x = 0;
    if (i >= 0) {
        c->a.x = w->props[i].value;
        w->props[i] = w->props[--w->nprops];
    }
    api_ret(c, n);
}

/* ---- setup -------------------------------------------------------------------------------- */

void user_init(void)
{
    g_arrow = user_load_system_cursor(32512);
    WndClass *k = class_add("#32769", 0, builtin_proc("DesktopProc"), 0, 0, 0, g_arrow, 0);
    k->builtin = 1;
    Wnd *d = calloc(1, sizeof(Wnd));
    d->hwnd = h_alloc(HT_WND, d);
    d->cls = k;
    d->style = WS_VISIBLE | WS_CLIPCHILDREN | WS_POPUP;
    d->wr = d->cr = irect(0, 0, SCREEN_W, SCREEN_H);
    d->wndproc = k->wndproc;
    d->text = calloc(1, 1);
    d->extra = calloc(1, 4);
    rgn_init(&d->update);
    g_desktop = d->hwnd;
    static const char *const builtin[] = { "BUTTON", "STATIC", "EDIT", "LISTBOX", "SCROLLBAR", "COMBOBOX",
                                           "MDICLIENT", "#32770", NULL };
    for (int i = 0; builtin[i]; i++) {
        const char *proc = !strcmp(builtin[i], "BUTTON") ? "ButtonProc"
                           : !strcmp(builtin[i], "STATIC") ? "StaticProc"
                           : !strcmp(builtin[i], "EDIT") ? "EditProc"
                                                          : "DefaultControlProc";
        WndClass *b = class_add(builtin[i], 0x0008 | 0x0003, builtin_proc(proc), 0,
                                !strcmp(builtin[i], "#32770") ? 30 : 0, 0, g_arrow, 0);
        b->builtin = 1;
    }
    wnd_invalidate(d, NULL, 1);
}

static void DesktopProc(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    uint32_t r = 0;
    if (a[1] == WM_ERASEBKGND) {
        DC *dc = dc_get((uint16_t)a[2]);
        if (dc)
            gdi_fill_rect(dc, irect(0, 0, SCREEN_W, SCREEN_H), gdi_stock(4 /* BLACK_BRUSH */));
        r = 1;
    } else {
        r = def_window_proc((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]);
    }
    api_ret32(c, n, r);
}

const ApiDef g_user_wnd_api[] = {
    { "USER", 18, "SetCapture", SetCapture },
    { "USER", 19, "ReleaseCapture", ReleaseCapture },
    { "USER", 22, "SetFocus", SetFocus },
    { "USER", 23, "GetFocus", GetFocus },
    { "USER", 24, "RemoveProp", RemoveProp },
    { "USER", 25, "GetProp", GetProp },
    { "USER", 26, "SetProp", SetProp },
    { "USER", 28, "ClientToScreen", ClientToScreen },
    { "USER", 29, "ScreenToClient", ScreenToClient },
    { "USER", 30, "WindowFromPoint", WindowFromPoint },
    { "USER", 31, "IsIconic", IsIconic },
    { "USER", 32, "GetWindowRect", GetWindowRect },
    { "USER", 33, "GetClientRect", GetClientRect },
    { "USER", 34, "EnableWindow", EnableWindow },
    { "USER", 35, "IsWindowEnabled", IsWindowEnabled },
    { "USER", 36, "GetWindowText", GetWindowText },
    { "USER", 37, "SetWindowText", SetWindowText },
    { "USER", 38, "GetWindowTextLength", GetWindowTextLength },
    { "USER", 39, "BeginPaint", BeginPaint },
    { "USER", 40, "EndPaint", EndPaint },
    { "USER", 41, "CreateWindow", CreateWindow },
    { "USER", 42, "ShowWindow", ShowWindow },
    { "USER", 45, "BringWindowToTop", BringWindowToTop },
    { "USER", 46, "GetParent", GetParent },
    { "USER", 47, "IsWindow", IsWindow },
    { "USER", 48, "IsChild", IsChild },
    { "USER", 49, "IsWindowVisible", IsWindowVisible },
    { "USER", 53, "DestroyWindow", DestroyWindow },
    { "USER", 56, "MoveWindow", MoveWindow },
    { "USER", 57, "RegisterClass", RegisterClass },
    { "USER", 58, "GetClassName", GetClassName },
    { "USER", 59, "SetActiveWindow", SetActiveWindow },
    { "USER", 60, "GetActiveWindow", GetActiveWindow },
    { "USER", 102, "AdjustWindowRect", AdjustWindowRect },
    { "USER", 105, "FlashWindow", FlashWindow },
    { "USER", 107, "DefWindowProc", DefWindowProc },
    { "USER", 122, "CallWindowProc", CallWindowProc },
    { "USER", 124, "UpdateWindow", UpdateWindow },
    { "USER", 125, "InvalidateRect", InvalidateRect },
    { "USER", 126, "InvalidateRgn", InvalidateRgn },
    { "USER", 127, "ValidateRect", ValidateRect },
    { "USER", 128, "ValidateRgn", ValidateRgn },
    { "USER", 129, "GetClassWord", GetClassWord },
    { "USER", 130, "SetClassWord", SetClassWord },
    { "USER", 131, "GetClassLong", GetClassLong },
    { "USER", 132, "SetClassLong", SetClassLong },
    { "USER", 133, "GetWindowWord", GetWindowWord },
    { "USER", 134, "SetWindowWord", SetWindowWord },
    { "USER", 135, "GetWindowLong", GetWindowLong },
    { "USER", 136, "SetWindowLong", SetWindowLong },
    { "USER", 190, "GetUpdateRect", GetUpdateRect },
    { "USER", 191, "ChildWindowFromPoint", ChildWindowFromPoint },
    { "USER", 229, "GetTopWindow", GetTopWindow },
    { "USER", 232, "SetWindowPos", SetWindowPos },
    { "USER", 236, "GetCapture", GetCapture },
    { "USER", 237, "GetUpdateRgn", GetUpdateRgn },
    { "USER", 258, "MapWindowPoints", MapWindowPoints },
    { "USER", 262, "GetWindow", GetWindow },
    { "USER", 272, "IsZoomed", IsZoomed },
    { "USER", 275, "RepaintScreen", RepaintScreen },
    { "USER", 277, "GetDlgCtrlID", GetDlgCtrlID },
    { "USER", 286, "GetDesktopWindow", GetDesktopWindow },
    { "USER", 403, "UnregisterClass", UnregisterClass },
    { "USER", 404, "GetClassInfo", GetClassInfo },
    { "USER", 452, "CreateWindowEx", CreateWindowEx },
    { "USER", 454, "AdjustWindowRectEx", AdjustWindowRectEx },
    { "USER", 91, "GetDlgItem", GetDlgItem },
    { "USER", 0, "DesktopProc", DesktopProc },
    { "USER", 0, "DefaultControlProc", DefaultControlProc },
    { NULL, 0, NULL, NULL }
};
