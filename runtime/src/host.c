/* Host services SDL doesn't cover everywhere. On the Switch (libnx): mounting the romfs that
 * holds the bundled fonts, the log over nxlink, the system error dialog in place of message
 * boxes, and the system keyboard for typing a name. Elsewhere these fall back to SDL. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "win.h"

#ifdef __SWITCH__
#include <switch.h>
#include <arpa/inet.h>

#include <stdlib.h>

static int g_nxlink;

/* Homebrew runs inside the homebrew menu's process, which takes over again afterwards: give
 * back the GPU, audio and services however the game ends (returning, exit, rt_fatal). */
static void host_exit(void)
{
    SDL_Quit();
    if (g_nxlink)
        socketExit();
    romfsExit();
}

void host_init(void)
{
    romfsInit();
    if (__nxlink_host.s_addr) {                   /* started by `nxlink -s`: log to the PC */
        socketInitializeDefault();
        g_nxlink = nxlinkStdio() >= 0;
    }
    atexit(host_exit);
}

int host_has_console(void) { return g_nxlink; }

void host_message(int error, const char *text)
{
    LOGI("message: %s", text);
    ErrorApplicationConfig c;
    if (R_SUCCEEDED(errorApplicationCreate(&c, text, NULL)))
        errorApplicationShow(&c);
    (void)error;
}

int host_text_prompt(const char *header, const char *sub, const char *initial, int max_len, char *out, size_t cap)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return -1;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    if (sub)
        swkbdConfigSetSubText(&kbd, sub);
    swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, (u32)max_len);
    out[0] = 0;
    Result rc = swkbdShow(&kbd, out, cap);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0];
}
#else
void host_init(void)
{
}

int host_has_console(void) { return 0; }

void host_message(int error, const char *text)
{
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, "Cosmology of Kyoto", text,
                             NULL);
}

int host_text_prompt(const char *header, const char *sub, const char *initial, int max_len, char *out, size_t cap)
{
    (void)header, (void)sub, (void)initial, (void)max_len, (void)out, (void)cap;
    return -1;
}
#endif
