/* USER's built-in control classes: BUTTON, STATIC and a minimal EDIT, drawn in the Windows
 * 3.1 style. Director builds its alerts and dialogs from these (an ASISubWndClass window
 * with BUTTON and STATIC children), so they must paint and click like the real ones:
 * colours come from the parent through WM_CTLCOLOR, owner-drawn buttons ask the parent
 * through WM_DRAWITEM, and a click reports WM_COMMAND/BN_CLICKED.
 */
#include <string.h>

#include "gdi.h"

#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_DRAWITEM 0x002B
#define WM_CTLCOLOR 0x0019
#define BM_GETCHECK 0x0400
#define BM_SETCHECK 0x0401
#define BM_GETSTATE 0x0402
#define BM_SETSTATE 0x0403
#define BM_SETSTYLE 0x0404
#define CTLCOLOR_EDIT 1
#define CTLCOLOR_BTN 3
#define CTLCOLOR_STATIC 6

enum { BS_PUSHBUTTON, BS_DEFPUSHBUTTON, BS_CHECKBOX, BS_AUTOCHECKBOX, BS_RADIOBUTTON, BS_3STATE, BS_AUTO3STATE,
       BS_GROUPBOX, BS_USERBUTTON, BS_AUTORADIOBUTTON, BS_PUSHBOX, BS_OWNERDRAW };

static uint32_t ctl_param(Wnd *w, int type) { return w->hwnd | (uint32_t)type << 16; }

/* Ask the parent for the control's colours (it sets the DC's text/background colours and
 * returns a brush); 0 means use the system colours. */
static uint16_t ctl_brush(Wnd *w, uint16_t hdc, int type)
{
    uint16_t parent = w->parent ? w->parent : w->owner;
    uint16_t br = parent ? (uint16_t)wnd_send(parent, WM_CTLCOLOR, hdc, ctl_param(w, type)) : 0;
    if (!br || (br > 21 && h_type(br) != HT_BRUSH)) {
        DC *dc = dc_get(hdc);
        if (dc) {
            dc->text_color = user_sys_color(type == CTLCOLOR_BTN ? 18 : 8);
            dc->bk_color = user_sys_color(type == CTLCOLOR_BTN ? 15 : 5);
        }
        br = gdi_solid_brush(user_sys_color(type == CTLCOLOR_BTN ? 15 : 5));
    } else if (br <= 21) {
        br = gdi_solid_brush(user_sys_color(br - 1));
    }
    return br;
}

static void select_font(Wnd *w, uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (dc)
        dc->hfont = w->ctl_font ? w->ctl_font : gdi_stock(13);
}

static IRect client_rect(Wnd *w)
{
    IRect cr = wnd_screen_client(w);
    return irect(0, 0, cr.r - cr.l, cr.b - cr.t);
}

static void fill(DC *dc, IRect r, uint32_t color) { gdi_fill_rect(dc, r, gdi_solid_brush(color)); }

static void hline(DC *dc, int x0, int x1, int y, uint32_t color) { fill(dc, irect(x0, y, x1, y + 1), color); }
static void vline(DC *dc, int x, int y0, int y1, uint32_t color) { fill(dc, irect(x, y0, x + 1, y1), color); }

/* ---- BUTTON ---------------------------------------------------------------------------- */

static int button_kind(Wnd *w) { return (int)(w->style & 0x0F); }

static void button_paint(Wnd *w, uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (!dc)
        return;
    int kind = button_kind(w);
    IRect r = client_rect(w);
    int disabled = (w->style & WS_DISABLED) != 0;
    int pressed = w->ctl_pressed;
    if (kind == BS_OWNERDRAW) {
        CPU *c = g_cpu;
        uint16_t sp = c->sp.x;
        c->sp.x -= 26;
        uint16_t o = c->sp.x;
        memset(mem_ptr(c->ss, o), 0, 26);
        wr16(c->ss, o, 4);                               /* ODT_BUTTON */
        wr16(c->ss, o + 2, w->id);
        wr16(c->ss, o + 6, 1);                           /* ODA_DRAWENTIRE */
        wr16(c->ss, o + 8, (uint16_t)((pressed ? 1 : 0) | (disabled ? 4 : 0) | (g_focus == w->hwnd ? 0x10 : 0)));
        wr16(c->ss, o + 10, w->hwnd);
        wr16(c->ss, o + 12, hdc);
        wr_rect((uint32_t)c->ss << 16 | (uint16_t)(o + 14), r);
        uint16_t parent = w->parent ? w->parent : w->owner;
        if (parent)
            wnd_send(parent, WM_DRAWITEM, w->id, (uint32_t)c->ss << 16 | o);
        c->sp.x = sp;
        return;
    }
    select_font(w, hdc);
    uint16_t br = ctl_brush(w, hdc, CTLCOLOR_BTN);
    uint32_t text_color = disabled ? user_sys_color(17) : dc->text_color;
    if (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON || kind == BS_PUSHBOX || kind == BS_USERBUTTON) {
        uint32_t black = 0, face = user_sys_color(15), shadow = user_sys_color(16), light = user_sys_color(20);
        fill(dc, r, face);
        /* rounded black outline (corners left out), thicker for the default button */
        hline(dc, r.l + 1, r.r - 1, r.t, black);
        hline(dc, r.l + 1, r.r - 1, r.b - 1, black);
        vline(dc, r.l, r.t + 1, r.b - 1, black);
        vline(dc, r.r - 1, r.t + 1, r.b - 1, black);
        IRect in = irect(r.l + 1, r.t + 1, r.r - 1, r.b - 1);
        if (kind == BS_DEFPUSHBUTTON) {
            gdi_frame_rect(dc, in, gdi_solid_brush(black));
            in = irect(in.l + 1, in.t + 1, in.r - 1, in.b - 1);
        }
        if (pressed) {
            hline(dc, in.l, in.r, in.t, shadow);
            vline(dc, in.l, in.t, in.b, shadow);
        } else {
            hline(dc, in.l, in.r - 1, in.t, light);
            vline(dc, in.l, in.t, in.b - 1, light);
            hline(dc, in.l + 1, in.r, in.b - 1, shadow);
            vline(dc, in.r - 1, in.t + 1, in.b, shadow);
            hline(dc, in.l + 2, in.r - 1, in.b - 2, shadow);
            vline(dc, in.r - 2, in.t + 2, in.b - 1, shadow);
        }
        DC saved = *dc;
        dc->bk_mode = 1;                                 /* TRANSPARENT */
        dc->text_color = text_color;
        IRect tr = in;
        if (pressed)
            rect_offset(&tr, 1, 1);
        gdi_draw_text_box(dc, tr, w->text, 1, 0, 0, 1);
        dc->bk_mode = saved.bk_mode;
        dc->text_color = saved.text_color;
        return;
    }
    gdi_fill_rect(dc, r, br);
    int th;
    gdi_text_extent(dc, "X", 1, &th);
    if (kind == BS_GROUPBOX) {
        int y = r.t + th / 2;
        gdi_frame_rect(dc, irect(r.l, y, r.r, r.b), gdi_solid_brush(0));
        int tw = gdi_text_extent(dc, w->text, (int)strlen(w->text), NULL);
        if (tw) {
            gdi_fill_rect(dc, irect(r.l + 8, r.t, r.l + 12 + tw, r.t + th), br);
            dc->text_color = text_color;
            gdi_draw_text_box(dc, irect(r.l + 10, r.t, r.r, r.t + th), w->text, 0, 0, 0, 0);
        }
        return;
    }
    /* check box / radio button: a 13x13 box and the text beside it */
    int box = 13, by = r.t + (r.b - r.t - box) / 2;
    int left_text = (w->style & 0x20) != 0;              /* BS_LEFTTEXT */
    int bx = left_text ? r.r - box : r.l;
    IRect b = irect(bx, by, bx + box, by + box);
    fill(dc, b, 0xFFFFFF);
    gdi_frame_rect(dc, b, gdi_solid_brush(0));
    int radio = kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON;
    if (w->ctl_check == 1) {
        if (radio) {
            fill(dc, irect(b.l + 4, b.t + 4, b.r - 4, b.b - 4), 0);
        } else {
            Target t;
            if (dc_target(dc, &t))
                for (int i = 2; i < box - 2; i++) {
                    blit_fill(&t, irect(b.l + i + t.ox, b.t + i + t.oy, b.l + i + 1 + t.ox, b.t + i + 1 + t.oy), 0);
                    blit_fill(&t, irect(b.r - 1 - i + t.ox, b.t + i + t.oy, b.r - i + t.ox, b.t + i + 1 + t.oy), 0);
                }
        }
    } else if (w->ctl_check == 2) {
        fill(dc, irect(b.l + 2, b.t + 2, b.r - 2, b.b - 2), user_sys_color(16));
    }
    if (pressed)
        gdi_frame_rect(dc, irect(b.l + 1, b.t + 1, b.r - 1, b.b - 1), gdi_solid_brush(0));
    DC saved = *dc;
    dc->bk_mode = 1;
    dc->text_color = text_color;
    IRect tr = left_text ? irect(r.l, r.t, b.l - 4, r.b) : irect(b.r + 4, r.t, r.r, r.b);
    gdi_draw_text_box(dc, tr, w->text, 0, 0, 0, 1);
    dc->bk_mode = saved.bk_mode;
    dc->text_color = saved.text_color;
}

static void ctl_redraw(Wnd *w, void (*paint)(Wnd *, uint16_t))
{
    if (!wnd_visible(w))
        return;
    uint16_t hdc = gdi_screen_dc(w->hwnd, 0);
    paint(w, hdc);
    gdi_release_dc(hdc);
}

static void notify_parent(Wnd *w, uint16_t code)
{
    uint16_t parent = w->parent ? w->parent : w->owner;
    if (parent)
        wnd_send(parent, WM_COMMAND, w->id, w->hwnd | (uint32_t)code << 16);
}

static void button_click(Wnd *w)
{
    uint16_t hwnd = w->hwnd;
    int kind = button_kind(w);
    if (kind == BS_AUTOCHECKBOX)
        w->ctl_check = !w->ctl_check;
    else if (kind == BS_AUTO3STATE)
        w->ctl_check = (uint8_t)((w->ctl_check + 1) % 3);
    else if (kind == BS_AUTORADIOBUTTON) {
        /* uncheck the other auto radio buttons of the group (siblings) */
        Wnd *p = wnd_get(w->parent);
        for (uint16_t h = p ? p->child : 0; h; h = wnd_get(h)->next) {
            Wnd *s = wnd_get(h);
            if (s != w && s->cls == w->cls && button_kind(s) == BS_AUTORADIOBUTTON && s->ctl_check) {
                s->ctl_check = 0;
                ctl_redraw(s, button_paint);
            }
        }
        w->ctl_check = 1;
    }
    ctl_redraw(w, button_paint);
    if (wnd_get(hwnd))
        notify_parent(w, 0);                             /* BN_CLICKED */
}

static uint32_t button_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    int kind = button_kind(w);
    switch (msg) {
    case WM_PAINT: {
        int erase;
        IRect pr;
        uint16_t hdc = user_begin_paint(hwnd, &erase, &pr);
        button_paint(w, hdc);
        gdi_release_dc(hdc);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFONT:
        w->ctl_font = wp;
        if (lp)
            ctl_redraw(w, button_paint);
        return 0;
    case WM_GETFONT:
        return w->ctl_font;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (kind == BS_GROUPBOX)
            return 0;
        g_capture = hwnd;
        w->ctl_tracking = 1;
        w->ctl_pressed = 1;
        ctl_redraw(w, button_paint);
        return 0;
    case WM_MOUSEMOVE:
        if (w->ctl_tracking) {
            IRect r = client_rect(w);
            int in = rect_has_pt(&r, (int16_t)(lp & 0xFFFF), (int16_t)(lp >> 16));
            if (in != w->ctl_pressed) {
                w->ctl_pressed = (uint8_t)in;
                ctl_redraw(w, button_paint);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->ctl_tracking) {
            int was = w->ctl_pressed;
            w->ctl_tracking = 0;
            w->ctl_pressed = 0;
            if (g_capture == hwnd)
                g_capture = 0;
            if (was)
                button_click(w);
            else
                ctl_redraw(w, button_paint);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == 0x20 && kind != BS_GROUPBOX) {
            w->ctl_pressed = 1;
            ctl_redraw(w, button_paint);
        }
        return 0;
    case WM_KEYUP:
        if (wp == 0x20 && w->ctl_pressed) {
            w->ctl_pressed = 0;
            button_click(w);
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        ctl_redraw(w, button_paint);
        return 0;
    case WM_SETTEXT: {
        uint32_t r = def_window_proc(hwnd, msg, wp, lp);
        if ((w = wnd_get(hwnd)))
            ctl_redraw(w, button_paint);
        return r;
    }
    case WM_GETDLGCODE:
        if (kind == BS_DEFPUSHBUTTON)
            return 0x2000 | 0x0010;                      /* DLGC_BUTTON | DLGC_DEFPUSHBUTTON */
        if (kind == BS_PUSHBUTTON)
            return 0x2000 | 0x0020;                      /* DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON */
        if (kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON)
            return 0x2000 | 0x0040;                      /* DLGC_RADIOBUTTON */
        return 0x2000;
    case BM_GETCHECK:
        return w->ctl_check;
    case BM_SETCHECK:
        if (w->ctl_check != wp) {
            w->ctl_check = (uint8_t)wp;
            ctl_redraw(w, button_paint);
        }
        return 0;
    case BM_GETSTATE:
        return (uint32_t)(w->ctl_check | (w->ctl_pressed ? 4 : 0) | (g_focus == hwnd ? 8 : 0));
    case BM_SETSTATE:
        w->ctl_pressed = wp != 0;
        ctl_redraw(w, button_paint);
        return 0;
    case BM_SETSTYLE:
        w->style = (w->style & ~0xFFu) | (wp & 0xFF);
        if (lp)
            ctl_redraw(w, button_paint);
        return 0;
    }
    return def_window_proc(hwnd, msg, wp, lp);
}

/* ---- STATIC ----------------------------------------------------------------------------- */

static void static_paint(Wnd *w, uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (!dc)
        return;
    IRect r = client_rect(w);
    int ss = (int)(w->style & 0x0F);
    switch (ss) {
    case 4: fill(dc, r, user_sys_color(6)); return;       /* SS_BLACKRECT: window frame colour */
    case 5: fill(dc, r, user_sys_color(1)); return;       /* SS_GRAYRECT */
    case 6: fill(dc, r, user_sys_color(5)); return;       /* SS_WHITERECT */
    case 7: gdi_frame_rect(dc, r, gdi_solid_brush(user_sys_color(6))); return;
    case 8: gdi_frame_rect(dc, r, gdi_solid_brush(user_sys_color(1))); return;
    case 9: gdi_frame_rect(dc, r, gdi_solid_brush(user_sys_color(5))); return;
    case 3: return;                                       /* SS_ICON: not drawn yet */
    }
    select_font(w, hdc);
    uint16_t br = ctl_brush(w, hdc, CTLCOLOR_STATIC);
    if (ss != 0xB)                                        /* SS_SIMPLE doesn't erase */
        gdi_fill_rect(dc, r, br);
    int saved_mode = dc->bk_mode;
    dc->bk_mode = 1;
    if (w->style & WS_DISABLED)
        dc->text_color = user_sys_color(17);
    int align = ss == 1 ? 1 : ss == 2 ? 2 : 0;
    gdi_draw_text_box(dc, r, w->text, align, ss <= 2, (w->style & 0x80) != 0, 0);
    dc->bk_mode = saved_mode;
}

static uint32_t static_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    switch (msg) {
    case WM_PAINT: {
        int erase;
        IRect pr;
        uint16_t hdc = user_begin_paint(hwnd, &erase, &pr);
        static_paint(w, hdc);
        gdi_release_dc(hdc);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFONT:
        w->ctl_font = wp;
        if (lp)
            ctl_redraw(w, static_paint);
        return 0;
    case WM_GETFONT:
        return w->ctl_font;
    case WM_NCHITTEST:
        return (uint32_t)-1;                             /* HTTRANSPARENT: clicks reach the parent */
    case WM_SETTEXT: {
        uint32_t r = def_window_proc(hwnd, msg, wp, lp);
        if ((w = wnd_get(hwnd)))
            ctl_redraw(w, static_paint);
        return r;
    }
    case WM_GETDLGCODE:
        return 0x0100;                                   /* DLGC_STATIC */
    }
    return def_window_proc(hwnd, msg, wp, lp);
}

/* ---- EDIT (single line, enough for simple prompts) ---------------------------------------- */

static void edit_paint(Wnd *w, uint16_t hdc)
{
    DC *dc = dc_get(hdc);
    if (!dc)
        return;
    IRect r = client_rect(w);
    select_font(w, hdc);
    uint16_t br = ctl_brush(w, hdc, CTLCOLOR_EDIT);
    gdi_fill_rect(dc, r, br);
    if (w->style & WS_BORDER)
        gdi_frame_rect(dc, r, gdi_solid_brush(0));
    int saved_mode = dc->bk_mode;
    dc->bk_mode = 1;
    int th;
    int tw = gdi_text_extent(dc, w->text, (int)strlen(w->text), &th);
    IRect tr = irect(r.l + 2, r.t + 1, r.r - 2, r.b - 1);
    gdi_draw_text_box(dc, tr, w->text, 0, 0, 1, 1);
    if (g_focus == w->hwnd) {                            /* a caret at the end */
        int cy = tr.t + (tr.b - tr.t - th) / 2;
        fill(dc, irect(tr.l + tw, cy, tr.l + tw + 1, cy + th), dc->text_color);
    }
    dc->bk_mode = saved_mode;
}

static uint32_t edit_proc(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    switch (msg) {
    case WM_PAINT: {
        int erase;
        IRect pr;
        uint16_t hdc = user_begin_paint(hwnd, &erase, &pr);
        edit_paint(w, hdc);
        gdi_release_dc(hdc);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFONT:
        w->ctl_font = wp;
        return 0;
    case WM_GETFONT:
        return w->ctl_font;
    case WM_LBUTTONDOWN:
        if (g_focus != hwnd)
            user_set_focus(hwnd);
        return 0;
    case WM_CHAR: {
        size_t len = strlen(w->text);
        if (wp == 8) {
            if (len)
                w->text[len - 1] = 0;
        } else if (wp >= 0x20 && len < 255) {
            char *t = realloc(w->text, len + 2);
            t[len] = (char)wp;
            t[len + 1] = 0;
            w->text = t;
        } else {
            return 0;
        }
        ctl_redraw(w, edit_paint);
        notify_parent(w, 0x0300);                        /* EN_CHANGE */
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        ctl_redraw(w, edit_paint);
        return 0;
    case WM_SETTEXT: {
        uint32_t r = def_window_proc(hwnd, msg, wp, lp);
        if ((w = wnd_get(hwnd)))
            ctl_redraw(w, edit_paint);
        return r;
    }
    case WM_GETDLGCODE:
        return 0x0080 | 0x0008 | 0x0001;                 /* DLGC_WANTCHARS | HASSETSEL | WANTARROWS */
    }
    if (msg >= 0x0400 && msg < 0x0430)                   /* EM_xxx: accepted, nothing to report */
        return 0;
    return def_window_proc(hwnd, msg, wp, lp);
}

/* ---- the class procedures as API thunks ----------------------------------------------- */

#define CTL_THUNK(name, impl)                                                                   \
    static void name(CPU *c)                                                                    \
    {                                                                                           \
        uint32_t a[4];                                                                          \
        uint16_t n = api_args(c, "wwwl", a);                                                    \
        api_ret32(c, n, impl((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]));           \
    }

CTL_THUNK(ButtonProc, button_proc)
CTL_THUNK(StaticProc, static_proc)
CTL_THUNK(EditProc, edit_proc)

const ApiDef g_user_ctl_api[] = {
    { "USER", 0, "ButtonProc", ButtonProc },
    { "USER", 0, "StaticProc", StaticProc },
    { "USER", 0, "EditProc", EditProc },
    { NULL, 0, NULL, NULL }
};
