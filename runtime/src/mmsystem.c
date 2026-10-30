/* MMSYSTEM: timer queries and MCI (waveform output and timer events are in wave.c).
 *
 * MCI: no devices are installed yet, so every command fails the way it would on a machine
 * without the driver; Director then takes its fallback paths. The trace shows the strings.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "win.h"

static void timeGetTime(CPU *c) { api_ret32(c, 0, rt_ticks()); }

static void timeGetDevCaps(CPU *c)     /* UINT timeGetDevCaps(TIMECAPS FAR *, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    wr16(FAR_SEL(a[0]), FAR_OFF(a[0]), 1);
    wr16(FAR_SEL(a[0]), FAR_OFF(a[0]) + 2, 65535);
    c->a.x = 0;
    api_ret(c, n);
}

static void timeBeginPeriod(CPU *c) { uint32_t a[1]; uint16_t n = api_args(c, "w", a); c->a.x = 0; api_ret(c, n); }
static void timeEndPeriod(CPU *c) { uint32_t a[1]; uint16_t n = api_args(c, "w", a); c->a.x = 0; api_ret(c, n); }

#define MCIERR_DEVICE_NOT_INSTALLED 0x0132     /* MCIERR_BASE (256) + 50 */

/* DWORD mciSendString(LPCSTR cmd, LPSTR ret, UINT size, HWND callback) */
static void mciSendString(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llww", a);
    LOGI("mciSendString(\"%s\"): no MCI devices", guest_ptr(a[0]));
    if (a[1] && a[2])
        guest_ptr(a[1])[0] = 0;
    api_ret32(c, n, MCIERR_DEVICE_NOT_INSTALLED);
}

/* DWORD mciSendCommand(UINT device, UINT msg, DWORD flags, DWORD params) */
static void mciSendCommand(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "wwll", a);
    const char *dev = "";
    if (a[1] == 0x0803 && a[3] && (a[2] & 0x2000 /* MCI_OPEN_TYPE */)) {
        uint32_t type = rd32(FAR_SEL(a[3]), FAR_OFF(a[3]) + 8);       /* lpstrDeviceType */
        dev = (a[2] & 0x1000 /* MCI_OPEN_TYPE_ID */) || !FAR_SEL(type) ? "(by id)" : guest_ptr(type);
    }
    LOGI("mciSendCommand(%u, %04x, %08x) %s: no MCI devices", a[0], a[1], a[2], dev);
    api_ret32(c, n, MCIERR_DEVICE_NOT_INSTALLED);
}

/* BOOL mciGetErrorString(DWORD err, LPSTR, UINT) */
static void mciGetErrorString(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "llw", a);
    if (a[2]) {
        snprintf(guest_ptr(a[1]), a[2], "%s", "The specified device is not installed.");
    }
    c->a.x = 1;
    api_ret(c, n);
}

static void midiOutGetNumDevs(CPU *c) { c->a.x = 0; api_ret(c, 0); }

const ApiDef g_mmsystem_api[] = {
    { "MMSYSTEM", 201, "midiOutGetNumDevs", midiOutGetNumDevs },
    { "MMSYSTEM", 604, "timeGetDevCaps", timeGetDevCaps },
    { "MMSYSTEM", 605, "timeBeginPeriod", timeBeginPeriod },
    { "MMSYSTEM", 606, "timeEndPeriod", timeEndPeriod },
    { "MMSYSTEM", 607, "timeGetTime", timeGetTime },
    { "MMSYSTEM", 701, "mciSendCommand", mciSendCommand },
    { "MMSYSTEM", 702, "mciSendString", mciSendString },
    { "MMSYSTEM", 706, "mciGetErrorString", mciGetErrorString },
    { NULL, 0, NULL, NULL }
};
