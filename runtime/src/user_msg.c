/* USER messages: the application queue, input, timers, and calling window procedures.
 *
 * Win16 has one queue per task; with one task there is one queue. Retrieval order follows
 * Windows: posted messages, then input, then WM_PAINT, then WM_TIMER, and WM_QUIT once the
 * rest is empty. Input arrives raw from video.c and becomes mouse or keyboard messages
 * when retrieved (target window, hit test and WM_SETCURSOR are decided then, as in Windows).
 * Window procedures are guest code called through rt_callback with the Pascal argument
 * order; built-in classes' procedures are API thunks, so the same path reaches them.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "win.h"

CPU *g_cpu;
uint16_t g_focus, g_active, g_capture;
int g_mouse_x = SCREEN_W / 2, g_mouse_y = SCREEN_H / 2;
uint8_t g_key_state[256];                  /* as of the last retrieved message (GetKeyState) */
static uint8_t g_async[256];               /* bit 7 down now, bit 0 pressed since last query */

uint32_t rt_ticks(void) { return (uint32_t)SDL_GetTicks(); }
static uint32_t msg_time(void) { return rt_ticks(); }

/* ---- calling window procedures -------------------------------------------------------------- */

uint32_t wnd_call_proc(uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    CPU *c = g_cpu;
    if (!proc)
        return 0;
    uint16_t ds = c->ds, es = c->es, sp = c->sp.x;
    uint16_t bx = c->b.x, cx = c->c.x;
    Wnd *w = wnd_get(hwnd);
    uint16_t inst = w && w->hinst ? w->hinst : g_task->hmodule;
    push16(c, hwnd);
    push16(c, msg);
    push16(c, wp);
    push32(c, lp);
    /* exported callbacks start with push ds / pop ax (or get AX from an instance thunk):
     * enter with both holding the instance's data segment */
    c->ds = c->es = (uint16_t)(inst | 7) == (uint16_t)(g_task->hmodule | 7) ? g_task->hmodule : inst;
    c->a.x = c->ds;
    rt_callback(c, FAR_SEL(proc), FAR_OFF(proc));
    uint32_t r = c->a.x | (uint32_t)c->d.x << 16;
    if (c->sp.x != sp) {
        LOGW("window procedure %s for message %04x left SP off by %d", rt_where(FAR_SEL(proc), FAR_OFF(proc)), msg,
             (int16_t)(c->sp.x - sp));
        c->sp.x = sp;
    }
    c->ds = ds;
    c->es = es;
    c->b.x = bx;
    c->c.x = cx;
    return r;
}

uint32_t wnd_send(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    return wnd_call_proc(w->wndproc, hwnd, msg, wp, lp);
}

/* ---- the posted-message queue --------------------------------------------------------------- */

typedef struct QMsg {
    uint16_t hwnd, msg, wp;
    uint32_t lp, time;
    int16_t x, y;
} QMsg;

#define QSIZE 512
static QMsg g_post[QSIZE];
static int g_npost;
static int g_quit, g_quit_code;
static uint32_t g_last_time;
static int16_t g_last_x, g_last_y;

int msg_post(uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp)
{
    if (g_npost == QSIZE) {
        LOGW("message queue full; dropping %04x", msg);
        return 0;
    }
    g_post[g_npost++] = (QMsg){ hwnd, msg, wp, lp, msg_time(), (int16_t)g_mouse_x, (int16_t)g_mouse_y };
    return 1;
}

static int msg_matches(const QMsg *m, uint16_t hwnd, uint16_t lo, uint16_t hi)
{
    if (hwnd && m->hwnd != hwnd)
        return 0;
    if (lo || hi)
        return m->msg >= lo && m->msg <= hi;
    return 1;
}

/* ---- raw input -------------------------------------------------------------------------- */

typedef struct Input {
    int kind, a, b, x, y;
    uint32_t time;
} Input;

#define ISIZE 256
static Input g_in[ISIZE];
static int g_nin;

static void async_set(int vk, int down)
{
    if (down)
        g_async[vk] |= 0x81;
    else
        g_async[vk] &= (uint8_t)~0x80;
}

void input_event(int kind, int a, int b, int x, int y)
{
    static const int button_vk[3] = { 0x01, 0x02, 0x04 };
    switch (kind) {
    case IN_QUIT:
        /* closing the host window: ask the program to close its main window */
        if (g_active)
            msg_post(g_active, WM_CLOSE, 0, 0);
        else
            g_quit = 1;
        return;
    case IN_MOUSE_MOVE:
        g_mouse_x = x;
        g_mouse_y = y;
        video_dirty();
        /* coalesce consecutive moves, as Windows does */
        if (g_nin && g_in[g_nin - 1].kind == IN_MOUSE_MOVE) {
            g_in[g_nin - 1].x = x;
            g_in[g_nin - 1].y = y;
            return;
        }
        break;
    case IN_MOUSE_DOWN: case IN_MOUSE_UP:
        g_mouse_x = x;
        g_mouse_y = y;
        async_set(button_vk[a], kind == IN_MOUSE_DOWN);
        break;
    case IN_KEY_DOWN: case IN_KEY_UP:
        async_set(a, kind == IN_KEY_DOWN);
        break;
    }
    if (g_nin == ISIZE)
        return;
    g_in[g_nin++] = (Input){ kind, a, b, x, y, msg_time() };
}

static uint16_t mouse_keys(void)
{
    uint16_t k = 0;
    if (g_key_state[0x01] & 0x80) k |= 0x01;
    if (g_key_state[0x02] & 0x80) k |= 0x02;
    if (g_key_state[0x10] & 0x80) k |= 0x04;
    if (g_key_state[0x11] & 0x80) k |= 0x08;
    if (g_key_state[0x04] & 0x80) k |= 0x10;
    return k;
}

static void key_state_set(int vk, int down)
{
    if (down) {
        if (!(g_key_state[vk] & 0x80))
            g_key_state[vk] ^= 1;                /* toggle on each press */
        g_key_state[vk] |= 0x80;
    } else {
        g_key_state[vk] &= (uint8_t)~0x80;
    }
}

static struct { uint32_t time; int x, y, button; uint16_t hwnd; } g_last_click;

/* Turn the oldest raw input event into a message. Returns 0 if it produced nothing (e.g.
 * a click on a disabled window); the event is consumed either way when `remove`. */
static int input_to_msg(const Input *in, QMsg *out)
{
    memset(out, 0, sizeof(*out));
    out->time = in->time;
    out->x = (int16_t)in->x;
    out->y = (int16_t)in->y;
    if (in->kind == IN_KEY_DOWN || in->kind == IN_KEY_UP) {
        int down = in->kind == IN_KEY_DOWN;
        int alt = (g_key_state[0x12] & 0x80) || (in->a == 0x12 && down);
        uint16_t target = g_focus ? g_focus : g_active;
        if (!target)
            return 0;
        int sys = alt || !g_focus;
        if ((g_key_state[0x11] & 0x80) && in->a != 0x12)
            sys = !g_focus;                       /* Ctrl+Alt combinations are plain keys */
        out->hwnd = target;
        out->msg = (uint16_t)(down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP));
        out->wp = (uint16_t)in->a;
        int repeat = (in->b & 0x100) != 0;
        out->lp = 1 | (uint32_t)(in->b & 0xFF) << 16 | (alt ? 1u << 29 : 0) |
                  (repeat || !down ? 1u << 30 : 0) | (down ? 0 : 1u << 31);
        return 1;
    }
    /* mouse */
    uint16_t hwnd = g_capture ? g_capture : wnd_from_point(in->x, in->y);
    Wnd *w = wnd_get(hwnd);
    if (!w)
        return 0;
    uint16_t hit = HTCLIENT;
    if (!g_capture)
        hit = (uint16_t)wnd_send(hwnd, WM_NCHITTEST, 0, (uint32_t)(uint16_t)in->x | (uint32_t)(uint16_t)in->y << 16);
    if ((w->style & WS_DISABLED) || hit == 0xFFFE /* HTERROR */) {
        return 0;
    }
    static const uint16_t base[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
    uint16_t msg;
    if (in->kind == IN_MOUSE_MOVE)
        msg = WM_MOUSEMOVE;
    else if (in->kind == IN_MOUSE_DOWN)
        msg = base[in->a];
    else
        msg = (uint16_t)(base[in->a] + 1);
    if (in->kind == IN_MOUSE_DOWN) {
        int dbl = (w->cls->style & 0x0008 /* CS_DBLCLKS */) && g_last_click.hwnd == hwnd &&
                  g_last_click.button == in->a && in->time - g_last_click.time <= 452 &&
                  SDL_abs(in->x - g_last_click.x) <= 4 && SDL_abs(in->y - g_last_click.y) <= 4;
        if (dbl) {
            msg = (uint16_t)(msg + 2);
            g_last_click.hwnd = 0;
        } else {
            g_last_click.time = in->time;
            g_last_click.x = in->x;
            g_last_click.y = in->y;
            g_last_click.button = in->a;
            g_last_click.hwnd = hwnd;
        }
    }
    if (!g_capture)
        wnd_send(hwnd, WM_SETCURSOR, hwnd, hit | (uint32_t)msg << 16);
    out->hwnd = hwnd;
    if (hit == HTCLIENT) {
        IRect cr = wnd_screen_client(w);
        out->msg = msg;
        out->lp = (uint32_t)(uint16_t)(in->x - cr.l) | (uint32_t)(uint16_t)(in->y - cr.t) << 16;
    } else {
        out->msg = (uint16_t)(msg - WM_MOUSEMOVE + WM_NCMOUSEMOVE);
        out->wp = hit;
        out->lp = (uint32_t)(uint16_t)in->x | (uint32_t)(uint16_t)in->y << 16;
    }
    return 1;
}

/* ---- timers ------------------------------------------------------------------------------ */

typedef struct Timer {
    uint16_t hwnd, id;
    uint32_t proc;
    uint32_t elapse;
    uint64_t next;
    int used;
} Timer;

#define MAX_TIMERS 32
#define TIMER_TICK 55                     /* Windows 3.1 timers run off the 18.2 Hz clock */
static Timer g_timer[MAX_TIMERS];

static uint64_t now_ms(void) { return SDL_GetTicks(); }

static uint16_t timer_set(uint16_t hwnd, uint16_t id, uint16_t elapse, uint32_t proc)
{
    uint32_t e = ((uint32_t)elapse + TIMER_TICK - 1) / TIMER_TICK * TIMER_TICK;
    if (e < TIMER_TICK)
        e = TIMER_TICK;
    Timer *t = NULL;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timer[i].used && hwnd && g_timer[i].hwnd == hwnd && g_timer[i].id == id)
            t = &g_timer[i];
    if (!t) {
        for (int i = 0; i < MAX_TIMERS && !t; i++)
            if (!g_timer[i].used)
                t = &g_timer[i];
        if (!t)
            return 0;
        if (!hwnd) {                              /* the system picks the id */
            static uint16_t next_id = 1;
            id = next_id++;
        }
    }
    *t = (Timer){ hwnd, id, proc, e, now_ms() + e, 1 };
    return id;
}

static int timer_kill(uint16_t hwnd, uint16_t id)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timer[i].used && g_timer[i].hwnd == hwnd && g_timer[i].id == id) {
            g_timer[i].used = 0;
            return 1;
        }
    return 0;
}

static int timer_due(QMsg *out, uint16_t hwnd, int remove)
{
    uint64_t now = now_ms();
    Timer *best = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timer[i];
        if (t->used && t->next <= now && (!hwnd || t->hwnd == hwnd) && (!best || t->next < best->next))
            best = t;
    }
    if (!best)
        return 0;
    *out = (QMsg){ best->hwnd, WM_TIMER, best->id, best->proc, (uint32_t)now, (int16_t)g_mouse_x, (int16_t)g_mouse_y };
    if (remove)
        best->next = now + best->elapse;        /* no backlog: one WM_TIMER at a time */
    return 1;
}

static int next_timer_wait(void)
{
    uint64_t now = now_ms(), soonest = now + 50;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timer[i].used && g_timer[i].next < soonest)
            soonest = g_timer[i].next;
    return soonest > now ? (int)(soonest - now) : 0;
}

/* ---- painting --------------------------------------------------------------------------- */

static uint16_t find_paint(uint16_t hwnd_filter, uint16_t start)
{
    for (uint16_t h = start; h; ) {
        Wnd *w = wnd_get(h);
        if (!w)
            break;
        if (w->style & WS_VISIBLE) {
            if (w->nc_dirty) {
                w->nc_dirty = 0;
                wnd_send(h, WM_NCPAINT, 1, 0);
            }
            if (w->update.n && (!hwnd_filter || hwnd_filter == h))
                return h;
            uint16_t c = find_paint(hwnd_filter, w->child);
            if (c)
                return c;
        }
        h = w->next;
    }
    return 0;
}

/* ---- retrieval -------------------------------------------------------------------------- */

static uint64_t g_last_pump;

static void pump(int wait_ms)
{
    mm_poll();
    wait_ms = mm_wait_ms(wait_ms);
    uint64_t now = SDL_GetTicksNS();
    if (wait_ms > 0 || now - g_last_pump > SDL_NS_PER_MS) {
        g_last_pump = now;
        video_pump(wait_ms);
    }
    video_present(0);
}

void user_yield(void)
{
    pump(0);
}

static void set_msg_state(const QMsg *m)
{
    g_last_time = m->time;
    g_last_x = m->x;
    g_last_y = m->y;
    switch (m->msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: key_state_set(m->wp, 1); break;
    case WM_KEYUP: case WM_SYSKEYUP: key_state_set(m->wp, 0); break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: key_state_set(0x01, 1); break;
    case WM_LBUTTONUP: key_state_set(0x01, 0); break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: key_state_set(0x02, 1); break;
    case WM_RBUTTONUP: key_state_set(0x02, 0); break;
    case WM_MBUTTONDOWN: key_state_set(0x04, 1); break;
    case WM_MBUTTONUP: key_state_set(0x04, 0); break;
    }
}

/* One attempt to find a message. Returns 1 found, 0 none; WM_QUIT is returned as a message. */
static int get_one(QMsg *out, uint16_t hwnd, uint16_t lo, uint16_t hi, int remove)
{
    for (int i = 0; i < g_npost; i++) {
        if (msg_matches(&g_post[i], hwnd, lo, hi)) {
            *out = g_post[i];
            if (remove)
                memmove(&g_post[i], &g_post[i + 1], (size_t)(--g_npost - i) * sizeof(QMsg));
            return 1;
        }
    }
    while (g_nin) {
        Input in = g_in[0];
        QMsg m;
        /* key state must reflect earlier input before routing this event */
        int ok = input_to_msg(&in, &m);
        if (ok && !msg_matches(&m, hwnd, lo, hi)) {
            if (in.kind == IN_MOUSE_MOVE)
                ;                                 /* fall through to keep looking below */
            break;                                /* keep it queued for a later, wider call */
        }
        if (remove || !ok)
            memmove(&g_in[0], &g_in[1], (size_t)(--g_nin) * sizeof(Input));
        if (!ok)
            continue;
        if (m.msg >= WM_MOUSEMOVE && m.msg <= 0x020D)
            m.wp = mouse_keys() | (m.msg == WM_LBUTTONDOWN || m.msg == WM_LBUTTONDBLCLK ? 1 : 0) |
                   (m.msg == WM_RBUTTONDOWN || m.msg == WM_RBUTTONDBLCLK ? 2 : 0);
        *out = m;
        return 1;
    }
    if (!lo && !hi ? 1 : (WM_PAINT >= lo && WM_PAINT <= hi)) {
        uint16_t h = find_paint(hwnd, wnd_get(g_desktop) ? wnd_get(g_desktop)->child : 0);
        if (h) {
            *out = (QMsg){ h, WM_PAINT, 0, 0, msg_time(), (int16_t)g_mouse_x, (int16_t)g_mouse_y };
            return 1;
        }
    }
    if (!lo && !hi ? 1 : (WM_TIMER >= lo && WM_TIMER <= hi)) {
        if (timer_due(out, hwnd, remove))
            return 1;
    }
    if (g_quit) {
        *out = (QMsg){ 0, WM_QUIT, (uint16_t)g_quit_code, 0, msg_time(), (int16_t)g_mouse_x, (int16_t)g_mouse_y };
        if (remove)
            g_quit = 0;
        return 1;
    }
    return 0;
}

static void write_msg(uint32_t far, const QMsg *m)
{
    uint16_t s = FAR_SEL(far), o = FAR_OFF(far);
    wr16(s, o, m->hwnd);
    wr16(s, o + 2, m->msg);
    wr16(s, o + 4, m->wp);
    wr32(s, o + 6, m->lp);
    wr32(s, o + 10, m->time);
    wr16(s, o + 14, (uint16_t)m->x);
    wr16(s, o + 16, (uint16_t)m->y);
}

static void read_msg(uint32_t far, QMsg *m)
{
    uint16_t s = FAR_SEL(far), o = FAR_OFF(far);
    m->hwnd = rd16(s, o);
    m->msg = rd16(s, o + 2);
    m->wp = rd16(s, o + 4);
    m->lp = rd32(s, o + 6);
    m->time = rd32(s, o + 10);
    m->x = (int16_t)rd16(s, o + 14);
    m->y = (int16_t)rd16(s, o + 16);
}

static void GetMessage(CPU *c)         /* BOOL GetMessage(LPMSG, HWND, UINT, UINT) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "lwww", a);
    QMsg m;
    pump(0);
    while (!get_one(&m, (uint16_t)a[1], (uint16_t)a[2], (uint16_t)a[3], 1))
        pump(next_timer_wait());
    set_msg_state(&m);
    write_msg(a[0], &m);
    c->a.x = m.msg != WM_QUIT;
    api_ret(c, n);
}

static void PeekMessage(CPU *c)        /* BOOL PeekMessage(LPMSG, HWND, UINT, UINT, UINT flags) */
{
    uint32_t a[5];
    uint16_t n = api_args(c, "lwwww", a);
    QMsg m;
    pump(0);
    int remove = (a[4] & 1) != 0;
    int ok = get_one(&m, (uint16_t)a[1], (uint16_t)a[2], (uint16_t)a[3], remove);
    if (ok) {
        if (remove)
            set_msg_state(&m);
        write_msg(a[0], &m);
    }
    /* Director waits for its next frame by polling PeekMessage in a tight loop, which would
     * keep a host core at 100% all the time. When empty polls keep coming back to back, the
     * program is only waiting: sleep 1 ms now and then (input, timers and sound are handled
     * right after). A loop doing real work between polls never gets here. */
    static uint64_t last_empty;
    static int idle_polls;
    uint64_t now = SDL_GetTicksNS();
    if (ok || now - last_empty > 100 * SDL_NS_PER_US)
        idle_polls = 0;
    else if (++idle_polls >= 8) {
        idle_polls = 0;
        SDL_DelayNS(SDL_NS_PER_MS);              /* high-resolution: ~1 ms, not a 15 ms tick */
        pump(0);
        now = SDL_GetTicksNS();
    }
    if (!ok)
        last_empty = now;
    c->a.x = (uint16_t)ok;
    api_ret(c, n);
}

static void WaitMessage(CPU *c)
{
    QMsg m;
    pump(0);
    while (!get_one(&m, 0, 0, 0, 0))
        pump(next_timer_wait());
    api_ret(c, 0);
}

/* US keyboard layout */
static int vk_to_char(int vk, int shift, int caps, int ctrl)
{
    if (ctrl) {
        if (vk >= 'A' && vk <= 'Z')
            return vk - 'A' + 1;
        return vk == 0x08 || vk == 0x0D || vk == 0x1B ? vk : 0;
    }
    if (vk >= 'A' && vk <= 'Z')
        return (shift != caps) ? vk : vk + 32;
    if (vk >= '0' && vk <= '9')
        return shift ? ")!@#$%^&*("[vk - '0'] : vk;
    if (vk >= 0x60 && vk <= 0x69)
        return '0' + (vk - 0x60);
    switch (vk) {
    case 0x08: case 0x09: case 0x0D: case 0x1B: case 0x20: return vk;
    case 0x6A: return '*'; case 0x6B: return '+'; case 0x6D: return '-'; case 0x6E: return '.'; case 0x6F: return '/';
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    }
    return 0;
}

int user_vk_to_char(int vk, const uint8_t *state)
{
    return vk_to_char(vk, (state[0x10] & 0x80) != 0, (state[0x14] & 1) != 0, (state[0x11] & 0x80) != 0);
}

static void TranslateMessage(CPU *c)   /* BOOL TranslateMessage(const MSG FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    QMsg m;
    read_msg(a[0], &m);
    c->a.x = 0;
    if (m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN) {
        int ch = user_vk_to_char(m.wp, g_key_state);
        if (ch) {
            msg_post(m.hwnd, m.msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, (uint16_t)ch, m.lp);
            c->a.x = 1;
        }
    }
    api_ret(c, n);
}

static void DispatchMessage(CPU *c)    /* LONG DispatchMessage(const MSG FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    QMsg m;
    read_msg(a[0], &m);
    uint32_t r = 0;
    if (m.msg == WM_TIMER && m.lp)
        r = wnd_call_proc(m.lp, m.hwnd, m.msg, m.wp, m.time);
    else if (wnd_get(m.hwnd))
        r = wnd_send(m.hwnd, m.msg, m.wp, m.lp);
    api_ret32(c, n, r);
}

static void SendMessage(CPU *c)        /* LRESULT SendMessage(HWND, UINT, WPARAM, LPARAM) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    uint32_t r = 0;
    if (a[0] == 0xFFFF) {                          /* HWND_BROADCAST: top-level windows */
        Wnd *d = wnd_get(g_desktop);
        for (uint16_t h = d ? d->child : 0; h; ) {
            Wnd *w = wnd_get(h);
            uint16_t next = w ? w->next : 0;
            wnd_send(h, (uint16_t)a[1], (uint16_t)a[2], a[3]);
            h = next;
        }
    } else {
        r = wnd_send((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]);
    }
    api_ret32(c, n, r);
}

static void PostMessage(CPU *c)        /* BOOL PostMessage(HWND, UINT, WPARAM, LPARAM) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    c->a.x = (uint16_t)msg_post((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]);
    api_ret(c, n);
}

static void PostQuitMessage(CPU *c)    /* void PostQuitMessage(int) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    g_quit = 1;
    g_quit_code = (int16_t)a[0];
    api_ret(c, n);
}

static void GetMessageTime(CPU *c) { api_ret32(c, 0, g_last_time); }

static void GetMessagePos(CPU *c)
{
    api_ret32(c, 0, (uint32_t)(uint16_t)g_last_x | (uint32_t)(uint16_t)g_last_y << 16);
}

static void GetMessageExtraInfo(CPU *c) { api_ret32(c, 0, 0); }

static void InSendMessage(CPU *c)
{
    c->a.x = 0;
    api_ret(c, 0);
}

static void GetQueueStatus(CPU *c)     /* DWORD GetQueueStatus(UINT flags) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t st = (uint16_t)((g_npost ? 0x0008 : 0) | (g_nin ? 0x0007 : 0));
    api_ret32(c, n, (uint32_t)st << 16 | st);
}

static void GetInputState(CPU *c)
{
    pump(0);
    c->a.x = g_nin != 0;
    api_ret(c, 0);
}

static void SetMessageQueue(CPU *c)    /* BOOL SetMessageQueue(int size) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 1;
    api_ret(c, n);
}

/* ---- timers API -------------------------------------------------------------------------- */

static void SetTimer(CPU *c)           /* UINT SetTimer(HWND, UINT id, UINT elapse, TIMERPROC) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwwl", a);
    c->a.x = timer_set((uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], a[3]);
    LOGD("SetTimer(%04x, %u, %u ms, %04x:%04x) = %u", a[0], a[1], a[2], FAR_SEL(a[3]), FAR_OFF(a[3]), c->a.x);
    api_ret(c, n);
}

static void KillTimer(CPU *c)          /* BOOL KillTimer(HWND, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = (uint16_t)timer_kill((uint16_t)a[0], (uint16_t)a[1]);
    api_ret(c, n);
}

void user_kill_window_timers(uint16_t hwnd)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timer[i].used && g_timer[i].hwnd == hwnd)
            g_timer[i].used = 0;
}

static void GetTickCount(CPU *c) { api_ret32(c, 0, msg_time()); }

/* ---- keyboard state ---------------------------------------------------------------------- */

static void GetKeyState(CPU *c)        /* int GetKeyState(int vk) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint8_t s = g_key_state[a[0] & 0xFF];
    c->a.x = (uint16_t)((s & 0x80 ? 0xFF80 : 0) | (s & 1));
    api_ret(c, n);
}

static void GetAsyncKeyState(CPU *c)   /* int GetAsyncKeyState(int vk) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    pump(0);
    uint8_t *s = &g_async[a[0] & 0xFF];
    c->a.x = (uint16_t)((*s & 0x80 ? 0x8000 : 0) | (*s & 1));
    *s &= 0x80;
    api_ret(c, n);
}

static void GetKeyboardState(CPU *c)   /* void GetKeyboardState(BYTE FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    memcpy(guest_ptr(a[0]), g_key_state, 256);
    api_ret(c, n);
}

static void SetKeyboardState(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    memcpy(g_key_state, guest_ptr(a[0]), 256);
    api_ret(c, n);
}

/* ---- hooks: accepted and never called ----------------------------------------------------- */

static void SetWindowsHook(CPU *c)     /* HHOOK SetWindowsHook(int, HOOKPROC) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    LOGI("SetWindowsHook(%d, %04x:%04x): hooks are not called", (int16_t)a[0], FAR_SEL(a[1]), FAR_OFF(a[1]));
    api_ret32(c, n, a[1]);                         /* returns the previous hook: report "itself" */
}

static void UnhookWindowsHook(CPU *c)  /* BOOL UnhookWindowsHook(int, HOOKPROC) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    c->a.x = 1;
    api_ret(c, n);
}

static void DefHookProc(CPU *c)        /* LRESULT DefHookProc(int, WPARAM, LPARAM, HHOOK FAR *) */
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwll", a);
    api_ret32(c, n, 0);
}

static void CallMsgFilter(CPU *c)      /* BOOL CallMsgFilter(MSG FAR *, int) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    c->a.x = 0;
    api_ret(c, n);
}

const ApiDef g_user_msg_api[] = {
    { "USER", 10, "SetTimer", SetTimer },
    { "USER", 12, "KillTimer", KillTimer },
    { "USER", 13, "GetTickCount", GetTickCount },
    { "USER", 15, "GetCurrentTime", GetTickCount },
    { "USER", 6, "PostQuitMessage", PostQuitMessage },
    { "USER", 106, "GetKeyState", GetKeyState },
    { "USER", 108, "GetMessage", GetMessage },
    { "USER", 109, "PeekMessage", PeekMessage },
    { "USER", 110, "PostMessage", PostMessage },
    { "USER", 111, "SendMessage", SendMessage },
    { "USER", 112, "WaitMessage", WaitMessage },
    { "USER", 113, "TranslateMessage", TranslateMessage },
    { "USER", 114, "DispatchMessage", DispatchMessage },
    { "USER", 120, "GetMessageTime", GetMessageTime },
    { "USER", 119, "GetMessagePos", GetMessagePos },
    { "USER", 121, "SetWindowsHook", SetWindowsHook },
    { "USER", 123, "CallMsgFilter", CallMsgFilter },
    { "USER", 222, "GetKeyboardState", GetKeyboardState },
    { "USER", 223, "SetKeyboardState", SetKeyboardState },
    { "USER", 234, "UnhookWindowsHook", UnhookWindowsHook },
    { "USER", 235, "DefHookProc", DefHookProc },
    { "USER", 240, "InSendMessage", InSendMessage },
    { "USER", 249, "GetAsyncKeyState", GetAsyncKeyState },
    { "USER", 266, "SetMessageQueue", SetMessageQueue },
    { "USER", 288, "GetMessageExtraInfo", GetMessageExtraInfo },
    { "USER", 334, "GetQueueStatus", GetQueueStatus },
    { "USER", 335, "GetInputState", GetInputState },
    { NULL, 0, NULL, NULL }
};
