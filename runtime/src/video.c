/* The host side of the display: an SDL window showing the emulated 8-bit screen.
 *
 * GDI draws into g_screen (system palette indices); presenting converts through the
 * hardware palette (g_syspal) into a streaming texture scaled to the window with nearest
 * filtering. The Windows cursor is drawn in software on top, so it scales with the image.
 * Host events become USER input (user_msg.c): mouse, touch (as SDL's emulated mouse),
 * keyboard, a game controller driving the cursor, and an on-screen keyboard where there is
 * one (the Switch).
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

Surface g_screen;
uint8_t g_syspal[256][3];

static SDL_Window *g_window;
static int g_native_modal;                      /* a native dialog is up: keep the host cursor */
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static uint32_t *g_rgb;                      /* conversion buffer */
static int g_dirty = 1;
static uint64_t g_last_present;
static int g_host_cursor_hidden;
static const char *g_shot;                   /* COK_SHOT: debug screenshot file */
static uint64_t g_last_shot;
static SDL_Texture *g_upscaled;              /* sharp scaling: the screen at a whole multiple */
static int g_upscaled_k;
static SDL_FRect g_dst;                      /* where the screen goes, in render pixels */

int g_start_fullscreen = -1;

static int is_fullscreen(void) { return (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0; }

/* Alt+Enter / F11: borderless fullscreen on the window's display, remembered for next time. */
static void toggle_fullscreen(void)
{
    int on = !is_fullscreen();
    SDL_SetWindowFullscreen(g_window, on);
    setting_set("fullscreen", on ? "1" : "0");
    g_dirty = 1;
    LOGI("video: %s", on ? "fullscreen" : "windowed");
}

#define PRESENT_INTERVAL_NS (SDL_NS_PER_SECOND / 60)

void video_init(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        rt_fatal("SDL_Init: %s", SDL_GetError());
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        LOGW("game controllers: %s", SDL_GetError());
    g_screen.w = SCREEN_W;
    g_screen.h = SCREEN_H;
    g_screen.stride = SCREEN_W;
    g_screen.bits = SDL_calloc(1, SCREEN_W * SCREEN_H);
    g_rgb = SDL_calloc(4, SCREEN_W * SCREEN_H);

    /* the largest integer scale that fits comfortably on the desktop */
    int scale = 2;
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (dm) {
        scale = 1;
        while ((scale + 1) * SCREEN_W <= dm->w * 9 / 10 && (scale + 1) * SCREEN_H <= dm->h * 9 / 10)
            scale++;
    }
    int win_w = SCREEN_W * scale, win_h = SCREEN_H * scale;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __SWITCH__
    if (dm) {                                     /* the window is the screen: else it's stretched to it */
        win_w = dm->w;
        win_h = dm->h;
    }
    /* the Switch driver makes windows only once EGL is loaded, which an OpenGL window does */
    flags |= SDL_WINDOW_OPENGL;
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#endif
#ifdef COK_ICON_RESOURCE                           /* Windows: the game's icon, from cok.exe */
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
    if (!SDL_CreateWindowAndRenderer("Cosmology of Kyoto", win_w, win_h, flags, &g_window, &g_renderer))
        rt_fatal("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    if (!g_texture)
        rt_fatal("SDL_CreateTexture: %s", SDL_GetError());
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_NEAREST);
    g_shot = SDL_getenv("COK_SHOT");
    int fullscreen = g_start_fullscreen >= 0 ? g_start_fullscreen : setting_int("fullscreen", 0);
    if (fullscreen)
        SDL_SetWindowFullscreen(g_window, true);
    LOGI("video: %dx%d screen in a %dx%d window%s (renderer %s)", SCREEN_W, SCREEN_H, win_w, win_h,
         fullscreen ? ", fullscreen" : "", SDL_GetRendererName(g_renderer));
}

void video_dirty(void)
{
    g_dirty = 1;
}

static void draw_cursor(void)
{
    Cursor *cur = g_cursor && g_cursor_show >= 0 ? h_get(g_cursor, HT_CURSOR) : NULL;
    if (!cur)
        return;
    int x0 = g_mouse_x - cur->hot_x, y0 = g_mouse_y - cur->hot_y;
    for (int y = 0; y < cur->h; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= SCREEN_H)
            continue;
        for (int x = 0; x < cur->w; x++) {
            int sx = x0 + x;
            if (sx < 0 || sx >= SCREEN_W)
                continue;
            int i = y * cur->w + x;
            uint32_t *p = &g_rgb[sy * SCREEN_W + sx];
            if (cur->color && !cur->and_mask[i]) {
                const uint8_t *c = g_syspal[cur->color_bits[i]];
                *p = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
                continue;
            }
            int a = cur->and_mask[i], xo = cur->xor_mask[i];
            if (!a)
                *p = xo ? 0xFFFFFF : 0x000000;   /* black or white */
            else if (xo)
                *p ^= 0xFFFFFF;                  /* inverted */
        }
    }
}

void video_present(int force)
{
    uint64_t now = SDL_GetTicksNS();
    int shot_due = g_shot && now - g_last_shot > 2 * SDL_NS_PER_SECOND;
    if (!force && !shot_due && (!g_dirty || now - g_last_present < PRESENT_INTERVAL_NS))
        return;
    g_last_present = now;
    g_dirty = 0;
    uint32_t pal[256];
    for (int i = 0; i < 256; i++)
        pal[i] = (uint32_t)g_syspal[i][0] << 16 | (uint32_t)g_syspal[i][1] << 8 | g_syspal[i][2];
    const uint8_t *src = g_screen.bits;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        g_rgb[i] = pal[src[i]];
    draw_cursor();
    /* COK_SHOT=file.bmp: keep a screenshot of the latest frame (every 2 s), for debugging;
     * with a %d in the name (seconds since start), keep them all */
    if (shot_due) {
        g_last_shot = now;
        int numbered = strstr(g_shot, "%d") != NULL;
        char path[600];
        if (numbered)
            snprintf(path, sizeof(path), g_shot, (int)(now / SDL_NS_PER_SECOND));
        else
            snprintf(path, sizeof(path), "%s", g_shot);
        SDL_Surface *sf = SDL_CreateSurfaceFrom(SCREEN_W, SCREEN_H, SDL_PIXELFORMAT_XRGB8888, g_rgb, SCREEN_W * 4);
        if (sf) {
            SDL_SaveBMP(sf, path);
            SDL_DestroySurface(sf);
        }
        if (!numbered) {
            char dib[600];
            snprintf(dib, sizeof(dib), "%s.dib.bmp", g_shot);
            gdi_debug_dump_dib(dib);
        }
    }
    SDL_UpdateTexture(g_texture, NULL, g_rgb, SCREEN_W * 4);

    /* the largest 4:3 rectangle in the window. A whole-number scale is drawn with nearest
     * filtering; otherwise the screen is first enlarged with nearest filtering to the next
     * whole multiple and that is fitted with linear filtering ("sharp bilinear"), so pixels
     * stay crisp without the uneven widths plain nearest scaling gives at 2.25x and the like */
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    float s = SDL_min((float)ow / SCREEN_W, (float)oh / SCREEN_H);
    if (s <= 0)
        s = 1;
    float w = SCREEN_W * s, h = SCREEN_H * s;
    g_dst = (SDL_FRect){ SDL_floorf((ow - w) / 2), SDL_floorf((oh - h) / 2), SDL_floorf(w + 0.5f), SDL_floorf(h + 0.5f) };
    int k = (int)SDL_ceilf(s - 0.01f);
    int whole = s < 1 || SDL_fabsf(s - SDL_roundf(s)) < 0.01f;
    SDL_Texture *show = g_texture;
    if (!whole) {
        if (g_upscaled_k != k) {
            if (g_upscaled)
                SDL_DestroyTexture(g_upscaled);
            g_upscaled = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET,
                                           SCREEN_W * k, SCREEN_H * k);
            if (g_upscaled)
                SDL_SetTextureScaleMode(g_upscaled, SDL_SCALEMODE_LINEAR);
            g_upscaled_k = g_upscaled ? k : 0;
        }
        if (g_upscaled && SDL_SetRenderTarget(g_renderer, g_upscaled)) {
            SDL_RenderTexture(g_renderer, g_texture, NULL, NULL);
            SDL_SetRenderTarget(g_renderer, NULL);
            show = g_upscaled;
        }
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, show, NULL, &g_dst);
    const char *wshot = shot_due ? SDL_getenv("COK_SHOT_WINDOW") : NULL;   /* debugging: the scaled output */
    if (wshot) {
        SDL_Surface *sf = SDL_RenderReadPixels(g_renderer, NULL);
        if (sf) {
            SDL_SaveBMP(sf, wshot);
            SDL_DestroySurface(sf);
        }
    }
    SDL_RenderPresent(g_renderer);
}

void video_set_cursor_visible(int on)
{
    (void)on;
    g_dirty = 1;
}

/* ---- host input -> Win16 virtual keys ------------------------------------------------------ */

static int vk_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0)
        return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    default: return 0;
    }
}

/* PC/AT set 1 scan codes for lParam bits 16-23, for the keys programs commonly check. */
static int pc_scancode(int vk)
{
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    for (int i = 0; row1[i]; i++) if (vk == row1[i]) return 0x10 + i;
    for (int i = 0; row2[i]; i++) if (vk == row2[i]) return 0x1E + i;
    for (int i = 0; row3[i]; i++) if (vk == row3[i]) return 0x2C + i;
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    switch (vk) {
    case '0': return 0x0B; case 0x1B: return 0x01; case 0x08: return 0x0E; case 0x09: return 0x0F;
    case 0x0D: return 0x1C; case 0x11: return 0x1D; case 0x10: return 0x2A; case 0x12: return 0x38;
    case 0x20: return 0x39; case 0x26: return 0x48; case 0x28: return 0x50; case 0x25: return 0x4B;
    case 0x27: return 0x4D; case 0x24: return 0x47; case 0x23: return 0x4F; case 0x21: return 0x49;
    case 0x22: return 0x51; case 0x2D: return 0x52; case 0x2E: return 0x53;
    }
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);
    return 0;
}

static void to_screen(float wx, float wy, int *x, int *y)
{
    float px = wx, py = wy;                      /* window -> render pixels (high DPI) */
    SDL_RenderCoordinatesFromWindow(g_renderer, wx, wy, &px, &py);
    if (g_dst.w <= 0 || g_dst.h <= 0) {
        *x = *y = 0;
        return;
    }
    *x = (int)SDL_floorf((px - g_dst.x) * SCREEN_W / g_dst.w);
    *y = (int)SDL_floorf((py - g_dst.y) * SCREEN_H / g_dst.h);
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= SCREEN_W) *x = SCREEN_W - 1;
    if (*y >= SCREEN_H) *y = SCREEN_H - 1;
}

static void type_char(char ch, int ctrl);

/* The close button (and Cmd+Q on macOS, Alt+F4): the game ignores Windows' close requests
 * (it sets Director's exitLock) and quits through its own Ctrl+Q, which offers to save. So
 * the first request presses Ctrl+Q for it; a second one quits at once, for the screens
 * that ignore Ctrl+Q (half a second apart, so a double click doesn't skip the save prompt). */
static void close_requested(void)
{
    static uint64_t first;
    uint64_t now = SDL_GetTicks();
    if (first && now - first >= 500) {
        LOGI("close requested again: quitting");
        SDL_Quit();
        exit(0);
    }
    if (!first) {
        LOGI("close requested: asking the game to quit (Ctrl+Q)");
        first = now;
        type_char('q', 1);
    }
}

/* ---- game controllers: the left stick (or D-pad) moves the cursor, A clicks, B right-clicks,
 * + (Start) asks to quit, Y brings up the on-screen keyboard -------------------------------- */

static float g_stick[2], g_dpad[2];               /* -1..1 */
static int g_pad_fast;                            /* ZR held: move faster */
static float g_cursor_fx, g_cursor_fy;            /* the cursor, to sub-pixel precision */
static uint64_t g_pad_last;                       /* last cursor update (ns), 0 when still */

static void pad_added(SDL_JoystickID id)
{
    /* devkitPro's SDL reports the Switch's controllers as a plain joystick: give it a
     * mapping (buttons in libnx's order: A B X Y, stick clicks, L R ZL ZR, + -, D-pad) */
    const char *name = SDL_GetJoystickNameForID(id);
    if (!SDL_IsGamepad(id) && name && !strcmp(name, "Switch Controller")) {
        char guid[64], map[400];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
        snprintf(map, sizeof(map),
                 "%s,Switch Controller,a:b0,b:b1,x:b2,y:b3,leftstick:b4,rightstick:b5,leftshoulder:b6,"
                 "rightshoulder:b7,lefttrigger:b8,righttrigger:b9,start:b10,back:b11,dpleft:b12,dpup:b13,"
                 "dpright:b14,dpdown:b15,leftx:a0,lefty:a1,rightx:a2,righty:a3,", guid);
        SDL_AddGamepadMapping(map);
    }
    if (SDL_IsGamepad(id) && !SDL_GetGamepadFromID(id) && SDL_OpenGamepad(id))
        LOGI("game controller: %s", SDL_GetGamepadNameForID(id));
}

static void pad_axis(int axis, int value)
{
    float v = value / 32767.0f;
    if (axis == SDL_GAMEPAD_AXIS_LEFTX)
        g_stick[0] = v;
    else if (axis == SDL_GAMEPAD_AXIS_LEFTY)
        g_stick[1] = v;
    else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        g_pad_fast = v > 0.5f;
}

static void close_requested(void);
static void show_keyboard(void);

static void pad_button(SDL_JoystickID id, int button, int down)
{
    /* by label, not position: A is the right-hand button on Nintendo controllers */
    SDL_GamepadButtonLabel label = SDL_GetGamepadButtonLabel(SDL_GetGamepadFromID(id), (SDL_GamepadButton)button);
    if (label == SDL_GAMEPAD_BUTTON_LABEL_A || label == SDL_GAMEPAD_BUTTON_LABEL_B) {
        input_event(down ? IN_MOUSE_DOWN : IN_MOUSE_UP, label == SDL_GAMEPAD_BUTTON_LABEL_A ? 0 : 1, 0, g_mouse_x,
                    g_mouse_y);
        return;
    }
    if (label == SDL_GAMEPAD_BUTTON_LABEL_Y) {
        if (down)
            show_keyboard();
        return;
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_START:
        if (down)
            close_requested();
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: g_dpad[0] = down ? -1 : 0; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: g_dpad[0] = down ? 1 : 0; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: g_dpad[1] = down ? -1 : 0; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: g_dpad[1] = down ? 1 : 0; break;
    }
}

/* Move the cursor by the stick's deflection: slow near the centre for aiming, a screen width
 * a second at full tilt (twice that with ZR); the D-pad creeps, for fine adjustment. */
static void pad_move_cursor(void)
{
    float dx = 0, dy = 0;
    float mag = SDL_sqrtf(g_stick[0] * g_stick[0] + g_stick[1] * g_stick[1]);
    if (mag > 0.15f) {
        float speed = (mag - 0.15f) / 0.85f;
        speed = speed * speed * SCREEN_W / mag;
        dx = g_stick[0] * speed;
        dy = g_stick[1] * speed;
    } else if (g_dpad[0] || g_dpad[1]) {
        dx = g_dpad[0] * 40.0f;
        dy = g_dpad[1] * 40.0f;
    }
    uint64_t now = SDL_GetTicksNS();
    if (dx == 0 && dy == 0) {
        g_pad_last = 0;
        return;
    }
    if (!g_pad_last) {                            /* starting: from wherever the cursor is now */
        g_pad_last = now;
        g_cursor_fx = g_mouse_x + 0.5f;
        g_cursor_fy = g_mouse_y + 0.5f;
        return;
    }
    float dt = (float)(now - g_pad_last) / SDL_NS_PER_SECOND;
    g_pad_last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    float k = dt * (g_pad_fast ? 2.0f : 1.0f);
    g_cursor_fx = SDL_clamp(g_cursor_fx + dx * k, 0.0f, SCREEN_W - 0.01f);
    g_cursor_fy = SDL_clamp(g_cursor_fy + dy * k, 0.0f, SCREEN_H - 0.01f);
    int x = (int)g_cursor_fx, y = (int)g_cursor_fy;
    if (x != g_mouse_x || y != g_mouse_y)
        input_event(IN_MOUSE_MOVE, 0, 0, x, y);
}

/* ---- the on-screen keyboard (the Switch): shown when the game wants text ------------------ */

/* Director draws its own text cursor, but asks for the blink rate over and over while an
 * editable field has the focus: that is the cue to bring up the keyboard, once per field. */
static uint64_t g_caret_poll_ns, g_caret_since_ns;
static int g_keyboard_offered;

void video_caret_polled(void)
{
    uint64_t now = SDL_GetTicksNS();
    if (!g_caret_poll_ns || now - g_caret_poll_ns > SDL_NS_PER_SECOND)
        g_caret_since_ns = now;                   /* a new field */
    g_caret_poll_ns = now;
}

static void show_keyboard(void)
{
    /* the system keyboard where there is one (the Switch: SDL's inline keyboard doesn't show
     * when homebrew runs from the album), its text typed into the game */
    char text[64];
    LOGI("on-screen keyboard");
    int r = host_text_prompt("Cosmology of Kyoto", NULL, "", 32, text, sizeof(text));
    if (r >= 0) {
        if (r) {
            LOGI("on-screen keyboard: \"%s\"", text);
            for (const char *p = text; *p; p++)
                type_char(*p, 0);
        }
        if (g_caret_poll_ns)                      /* the game was paused meanwhile: same field */
            g_caret_poll_ns = SDL_GetTicksNS();
        return;
    }
    if (SDL_HasScreenKeyboardSupport() && !SDL_ScreenKeyboardShown(g_window))
        SDL_StartTextInput(g_window);
}

static void keyboard_check(void)
{
    if (!g_caret_poll_ns)
        return;
    uint64_t now = SDL_GetTicksNS();
    if (now - g_caret_poll_ns > SDL_NS_PER_SECOND) {           /* the field has gone */
        g_caret_poll_ns = 0;
        g_keyboard_offered = 0;
        return;
    }
    /* asked steadily for a quarter of a second: a field is waiting for text */
    if (!g_keyboard_offered && now - g_caret_since_ns > SDL_NS_PER_SECOND / 4) {
        g_keyboard_offered = 1;
        show_keyboard();
    }
}

static void handle(const SDL_Event *e)
{
    int x, y;
    switch (e->type) {
    case SDL_EVENT_QUIT:
        close_requested();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        to_screen(e->motion.x, e->motion.y, &x, &y);
        input_event(IN_MOUSE_MOVE, 0, 0, x, y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 :
                e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b < 0)
            break;
        to_screen(e->button.x, e->button.y, &x, &y);
        input_event(e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? IN_MOUSE_DOWN : IN_MOUSE_UP, b, 0, x, y);
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        /* Alt+Enter and F11 toggle fullscreen; the game never sees them */
        int enter = e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER;
        if (e->key.scancode == SDL_SCANCODE_F11 || (enter && (e->key.mod & SDL_KMOD_ALT))) {
            if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
                toggle_fullscreen();
            break;
        }
        int vk = vk_from_scancode(e->key.scancode);
        if (!vk)
            break;
        input_event(e->type == SDL_EVENT_KEY_DOWN ? IN_KEY_DOWN : IN_KEY_UP, vk,
                    pc_scancode(vk) | (e->key.repeat ? 0x100 : 0), g_mouse_x, g_mouse_y);
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        if (!g_host_cursor_hidden && !g_native_modal) {
            SDL_HideCursor();
            g_host_cursor_hidden = 1;
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (g_host_cursor_hidden) {
            SDL_ShowCursor();
            g_host_cursor_hidden = 0;
        }
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
        g_dirty = 1;
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_added(e->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        pad_axis(e->gaxis.axis, e->gaxis.value);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        pad_button(e->gbutton.which, e->gbutton.button, e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        break;
    case SDL_EVENT_TEXT_INPUT:                    /* from the on-screen keyboard */
        LOGI("on-screen keyboard: \"%s\"", e->text.text);
        for (const char *p = e->text.text; *p; p++)
            type_char(*p, 0);
        break;
    default:
        break;
    }
}

/* COK_CLICK=ms,x,y;ms,'text';... : scripted input for debugging without hands. "ms,x,y" is a
 * left click (held 150 ms) at screen x,y; "ms,'text'" types text ('\n' or '|' = Enter, "^x" = Ctrl+x). */
static int g_release[3];                          /* keys of a chord still held (typed by script or for a close request) */
static uint64_t g_release_at;

static void type_char(char ch, int ctrl)
{
    int vk = 0, shift = 0;
    if (ch >= 'a' && ch <= 'z')
        vk = ch - 32;
    else if (ch >= 'A' && ch <= 'Z')
        vk = ch, shift = 1;
    else if ((ch >= '0' && ch <= '9') || ch == ' ')
        vk = ch;
    else if (ch == '|' || ch == '\n')
        vk = 0x0D;
    else if (ch == '-' || ch == '.' || ch == ',' || ch == '\'')
        vk = ch == '-' ? 0xBD : ch == '.' ? 0xBE : ch == ',' ? 0xBC : 0xDE;   /* VK_OEM_MINUS etc. */
    if (!vk)
        return;
    if (shift)
        input_event(IN_KEY_DOWN, 0x10, 0x2A, g_mouse_x, g_mouse_y);
    if (ctrl)
        input_event(IN_KEY_DOWN, 0x11, 0x1D, g_mouse_x, g_mouse_y);
    input_event(IN_KEY_DOWN, vk, pc_scancode(vk), g_mouse_x, g_mouse_y);
    if (ctrl) {                                   /* a chord: let go a moment later, like a hand */
        g_release[0] = vk;
        g_release[1] = 0x11;
        g_release[2] = shift ? 0x10 : 0;
        g_release_at = SDL_GetTicks() + 200;
        return;
    }
    input_event(IN_KEY_UP, vk, pc_scancode(vk), g_mouse_x, g_mouse_y);
    if (shift)
        input_event(IN_KEY_UP, 0x10, 0x2A, g_mouse_x, g_mouse_y);
}

static void release_held_keys(void)
{
    if (!g_release_at || SDL_GetTicks() < g_release_at)
        return;
    for (int i = 0; i < 3; i++)
        if (g_release[i])
            input_event(IN_KEY_UP, g_release[i], g_release[i] == 0x11 ? 0x1D : g_release[i] == 0x10 ? 0x2A
                                                                                 : pc_scancode(g_release[i]),
                        g_mouse_x, g_mouse_y);
    g_release_at = 0;
}

static void scripted_input(void)
{
    static const char *script;
    static int init;
    if (!init) {
        init = 1;
        script = SDL_getenv("COK_CLICK");
    }
    release_held_keys();
    if (!script || !*script)
        return;
    int ms = 0, x = 0, y = 0, used = 0;
    static int held;
    if (sscanf(script, "%d,%n", &ms, &used) >= 1 && !strncmp(script + used, "QUIT", 4)) {
        if ((int)SDL_GetTicks() < ms)                 /* "ms,QUIT": the window's close button */
            return;
        LOGI("scripted close request");
        SDL_Event q;
        SDL_zero(q);
        q.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&q);
        script += used + 4;
        if (*script == ';')
            script++;
        return;
    }
    if (sscanf(script, "%d,%n", &ms, &used) >= 1 && script[used] == '\'') {
        if ((int)SDL_GetTicks() < ms)
            return;
        const char *t = script + used + 1, *end = strchr(t, '\'');
        if (!end) {
            script = NULL;
            return;
        }
        LOGI("scripted typing \"%.*s\"", (int)(end - t), t);
        for (const char *p = t; p < end; p++) {
            int ctrl = *p == '^' && p + 1 < end;     /* ^x: Ctrl+x */
            type_char(ctrl ? *++p : *p, ctrl);
        }
        script = end + 1;
    } else {
        if (sscanf(script, "%d,%d,%d%n", &ms, &x, &y, &used) < 3) {
            script = NULL;
            return;
        }
        if ((int)SDL_GetTicks() < ms + (held ? 150 : 0))
            return;
        if (!held) {                              /* press, and hold it like a hand would */
            LOGI("scripted click at %d,%d", x, y);
            input_event(IN_MOUSE_MOVE, 0, 0, x, y);
            input_event(IN_MOUSE_DOWN, 0, 0, x, y);
            held = 1;
            return;
        }
        input_event(IN_MOUSE_UP, 0, 0, x, y);
        held = 0;
        script += used;
    }
    if (*script == ';')
        script++;
}

SDL_Window *video_sdl_window(void) { return g_window; }

void video_native_modal(int on)
{
    g_native_modal = on;
    if (on && g_host_cursor_hidden) {
        SDL_ShowCursor();
        g_host_cursor_hidden = 0;
    }
}

void video_pump(int wait_ms)
{
    SDL_Event e;
    int got = 0;
    scripted_input();
    pad_move_cursor();
    keyboard_check();
    if (g_pad_last && wait_ms > 10)               /* the stick is moving the cursor: keep it smooth */
        wait_ms = 10;
    while (SDL_PollEvent(&e)) {
        handle(&e);
        got = 1;
    }
    if (!got && wait_ms > 0 && SDL_WaitEventTimeout(&e, wait_ms)) {
        handle(&e);
        while (SDL_PollEvent(&e))
            handle(&e);
    }
}
