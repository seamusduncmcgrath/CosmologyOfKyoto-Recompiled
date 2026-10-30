/* MMSYSTEM waveform output and multimedia timers on SDL3 audio.
 *
 * Director plays sound through MacroMix (recompiled MACROMIX.DLL), a software mixer: a
 * periodic multimedia timer mixes its channels into buffers that it queues with
 * waveOutWrite, and the device's "buffer done" callback hands them back. On Windows both
 * callbacks run at interrupt time. Guest code runs on one host thread here, so they are
 * delivered at safe points instead (mm_poll: after every API call, and while the message
 * loop waits), with the whole CPU state saved and restored around them like an interrupt.
 *
 * Each open device is an SDL audio stream in the program's format (SDL converts and
 * resamples). A buffer is done once the device has consumed the stream up to its end.
 */
#include <SDL3/SDL.h>
#include <string.h>

#include "win.h"

#define MAX_WAVES 4
#define MAX_QUEUED 64
#define MAX_TIMERS 16
#define WAVE_HANDLE_BASE 0x5700

#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_ALLOCATED 4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_NOTSUPPORTED 8
#define WAVERR_BADFORMAT 32
#define WAVERR_STILLPLAYING 33
#define WAVERR_UNPREPARED 34

#define WAVE_FORMAT_QUERY 0x0001
#define CALLBACK_TYPEMASK 0x00070000u
#define CALLBACK_WINDOW 0x00010000u
#define CALLBACK_TASK 0x00020000u
#define CALLBACK_FUNCTION 0x00030000u

#define MM_WOM_OPEN 0x3BB
#define MM_WOM_CLOSE 0x3BC
#define MM_WOM_DONE 0x3BD

/* WAVEHDR */
#define WH_DATA 0
#define WH_LENGTH 4
#define WH_FLAGS 16
#define WH_LOOPS 20
#define WHDR_DONE 0x01
#define WHDR_PREPARED 0x02
#define WHDR_BEGINLOOP 0x04
#define WHDR_ENDLOOP 0x08
#define WHDR_INQUEUE 0x10

typedef struct Wave {
    int open;
    SDL_AudioStream *stream;
    int bytes_per_sec, block_align;
    uint32_t callback, instance, flags;
    uint64_t written;                     /* bytes put into the stream since opening */
    struct { uint32_t hdr; uint64_t end; } q[MAX_QUEUED];
    int nq;
    int paused;
} Wave;

typedef struct Timer {
    uint16_t id;
    uint32_t delay, due, proc, user;
    int periodic;
} Timer;

static Wave g_wave[MAX_WAVES];
static Timer g_timer[MAX_TIMERS];
static int g_audio_ok = -1;               /* SDL audio initialised: -1 not yet tried */
static uint16_t g_next_timer = 1;
static uint32_t g_volume = 0xFFFFFFFFu;   /* waveOutSetVolume: low word left, high right */
static int g_mm_active;                      /* open devices + timers: anything to poll? */
static int g_in_irq;

static int audio_ok(void)
{
    if (g_audio_ok < 0) {
        g_audio_ok = SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;
        if (!g_audio_ok)
            LOGW("audio: %s; no sound", SDL_GetError());
    }
    return g_audio_ok;
}

static void count_active(void)
{
    g_mm_active = 0;
    for (int i = 0; i < MAX_WAVES; i++)
        g_mm_active += g_wave[i].open;
    for (int i = 0; i < MAX_TIMERS; i++)
        g_mm_active += g_timer[i].id != 0;
}

static Wave *wave_get(uint16_t h)
{
    unsigned i = (unsigned)(h - WAVE_HANDLE_BASE);
    return i < MAX_WAVES && g_wave[i].open ? &g_wave[i] : NULL;
}

static float wave_gain(void)
{
    uint32_t l = g_volume & 0xFFFF, r = g_volume >> 16;
    return (float)(l > r ? l : r) / 65535.0f;
}

/* ---- calling guest code as an interrupt would ------------------------------------------- */

/* Push Pascal arguments (first argument first): each is a word, or a dword when the
 * matching `dw` flag is set. */
static void irq_call(uint32_t proc, int nargs, const uint32_t *args, const int *dw)
{
    CPU *c = g_cpu;
    if (!proc || !c)
        return;
    CPU saved = *c;
    g_in_irq = 1;
    for (int i = 0; i < nargs; i++) {
        if (dw[i]) {
            push16(c, (uint16_t)(args[i] >> 16));
            push16(c, (uint16_t)args[i]);
        } else {
            push16(c, (uint16_t)args[i]);
        }
    }
    rt_callback(c, FAR_SEL(proc), FAR_OFF(proc));
    *c = saved;
    g_in_irq = 0;
}

static void notify(Wave *w, uint16_t msg, uint32_t p1)
{
    uint16_t h = (uint16_t)(WAVE_HANDLE_BASE + (w - g_wave));
    switch (w->flags & CALLBACK_TYPEMASK) {
    case CALLBACK_FUNCTION: {
        /* void CALLBACK waveOutProc(HWAVEOUT, UINT msg, DWORD instance, DWORD p1, DWORD p2) */
        uint32_t args[5] = { h, msg, w->instance, p1, 0 };
        static const int dw[5] = { 0, 0, 1, 1, 1 };
        irq_call(w->callback, 5, args, dw);
        break;
    }
    case CALLBACK_WINDOW:
        msg_post((uint16_t)w->callback, msg, h, p1);
        break;
    }
}

/* Hand back the buffers the device has finished with. */
static void wave_progress(Wave *w)
{
    int queued = SDL_GetAudioStreamQueued(w->stream);
    /* The resampler keeps a few input bytes back until more data arrives. When the device
     * has run dry with only those left, flush them out: the program waits for its last
     * buffer to be done before it sends more (MacroMix streaming dialogue from disk). */
    if (queued > 0 && queued <= 256 && w->nq && SDL_GetAudioStreamAvailable(w->stream) == 0) {
        SDL_FlushAudioStream(w->stream);
        queued = SDL_GetAudioStreamQueued(w->stream);
    }
    uint64_t played = w->written - (uint64_t)(queued > 0 ? queued : 0);
    while (w->open && w->nq && w->q[0].end <= played) {
        uint32_t hdr = w->q[0].hdr;
        memmove(&w->q[0], &w->q[1], (size_t)--w->nq * sizeof(w->q[0]));
        uint16_t s = FAR_SEL(hdr), o = FAR_OFF(hdr);
        wr32(s, o + WH_FLAGS, (rd32(s, o + WH_FLAGS) & ~(uint32_t)WHDR_INQUEUE) | WHDR_DONE);
        notify(w, MM_WOM_DONE, hdr);
    }
}

void mm_poll(void)
{
    if (!g_mm_active || g_in_irq)
        return;
    static uint64_t last;
    uint64_t now_ns = SDL_GetTicksNS();
    if (now_ns - last < SDL_NS_PER_MS / 2)
        return;
    last = now_ns;
    for (int i = 0; i < MAX_WAVES; i++)
        if (g_wave[i].open)
            wave_progress(&g_wave[i]);
    uint32_t now = rt_ticks();
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timer[i];
        if (!t->id || (int32_t)(now - t->due) < 0)
            continue;
        Timer fire = *t;
        if (t->periodic) {
            t->due += t->delay;
            if ((int32_t)(now - t->due) >= 0)          /* far behind: don't fire in a burst */
                t->due = now + t->delay;
        } else {
            t->id = 0;
            count_active();
        }
        /* void CALLBACK TimeProc(UINT id, UINT msg, DWORD user, DWORD dw1, DWORD dw2) */
        uint32_t args[5] = { fire.id, 0, fire.user, 0, 0 };
        static const int dw[5] = { 0, 0, 1, 1, 1 };
        irq_call(fire.proc, 5, args, dw);
    }
}

/* How long the message loop may sleep before something here needs attention. */
int mm_wait_ms(int wait_ms)
{
    if (!g_mm_active)
        return wait_ms;
    return wait_ms < 0 || wait_ms > 2 ? 2 : wait_ms;
}

/* ---- waveOut ------------------------------------------------------------------------------ */

static void waveOutGetNumDevs(CPU *c)  /* UINT waveOutGetNumDevs(void) */
{
    c->a.x = audio_ok() ? 1 : 0;
    api_ret(c, 0);
}

/* UINT waveOutGetDevCaps(UINT id, WAVEOUTCAPS FAR *, UINT size) */
static void waveOutGetDevCaps(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    uint8_t caps[48] = { 0 };
    wr16_host(caps + 0, 1);                       /* wMid: MM_MICROSOFT */
    wr16_host(caps + 2, 2);                       /* wPid: MM_WAVEOUT_MAPPER-ish */
    wr16_host(caps + 4, 0x0300);                  /* vDriverVersion */
    strcpy((char *)caps + 6, "SDL Audio");
    wr32_host(caps + 38, 0x0FFF);                 /* dwFormats: 11/22/44 kHz, mono/stereo, 8/16 bit */
    wr16_host(caps + 42, 2);                      /* wChannels */
    wr32_host(caps + 44, 0x0004 | 0x0008);        /* dwSupport: WAVECAPS_VOLUME | LRVOLUME */
    if (!audio_ok() || (a[0] != 0 && a[0] != 0xFFFF)) {
        c->a.x = MMSYSERR_BADDEVICEID;
    } else {
        memcpy(guest_ptr(a[1]), caps, a[2] < sizeof(caps) ? a[2] : sizeof(caps));
        c->a.x = 0;
    }
    api_ret(c, n);
}

/* UINT waveOutOpen(HWAVEOUT FAR *, UINT id, const WAVEFORMAT FAR *, DWORD callback,
 *                  DWORD instance, DWORD flags) */
static void waveOutOpen(CPU *c)
{
    uint32_t a[6];
    uint16_t n = api_args(c, "lwllll", a);
    uint16_t s = FAR_SEL(a[2]), o = FAR_OFF(a[2]);
    int tag = rd16(s, o), channels = rd16(s, o + 2), bits = rd16(s, o + 14);
    uint32_t rate = rd32(s, o + 4);
    int ok_format = tag == 1 && (channels == 1 || channels == 2) && (bits == 8 || bits == 16) && rate >= 4000 &&
                    rate <= 48000;
    if (!audio_ok() || (a[1] != 0 && a[1] != 0xFFFF)) {
        c->a.x = MMSYSERR_BADDEVICEID;
    } else if (!ok_format) {
        LOGW("waveOutOpen: format %d, %d ch, %u Hz, %d bit not supported", tag, channels, rate, bits);
        c->a.x = WAVERR_BADFORMAT;
    } else if (a[5] & WAVE_FORMAT_QUERY) {
        c->a.x = 0;
    } else {
        int i = 0;
        while (i < MAX_WAVES && g_wave[i].open)
            i++;
        SDL_AudioSpec spec = { bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE, channels, (int)rate };
        SDL_AudioStream *st = i < MAX_WAVES ? SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                                                         NULL, NULL)
                                            : NULL;
        if (!st) {
            LOGW("waveOutOpen: %s", i < MAX_WAVES ? SDL_GetError() : "too many devices open");
            c->a.x = MMSYSERR_ALLOCATED;
        } else {
            Wave *w = &g_wave[i];
            memset(w, 0, sizeof(*w));
            w->open = 1;
            w->stream = st;
            w->block_align = channels * bits / 8;
            w->bytes_per_sec = (int)rate * w->block_align;
            w->callback = a[3];
            w->instance = a[4];
            w->flags = a[5];
            SDL_SetAudioStreamGain(st, wave_gain());
            SDL_ResumeAudioStreamDevice(st);
            uint16_t h = (uint16_t)(WAVE_HANDLE_BASE + i);
            if (a[0])
                wr16(FAR_SEL(a[0]), FAR_OFF(a[0]), h);
            count_active();
            LOGI("waveOutOpen: %u Hz, %d bit, %d ch, callback %08x (type %x) = %04x", rate, bits, channels, a[3],
                 (a[5] & CALLBACK_TYPEMASK) >> 16, h);
            notify(w, MM_WOM_OPEN, 0);
            c->a.x = 0;
        }
    }
    api_ret(c, n);
}

static void waveOutClose(CPU *c)       /* UINT waveOutClose(HWAVEOUT) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wave *w = wave_get((uint16_t)a[0]);
    if (!w) {
        c->a.x = MMSYSERR_INVALHANDLE;
    } else if (w->nq) {
        c->a.x = WAVERR_STILLPLAYING;
    } else {
        SDL_DestroyAudioStream(w->stream);
        w->stream = NULL;
        notify(w, MM_WOM_CLOSE, 0);
        w->open = 0;
        count_active();
        c->a.x = 0;
    }
    api_ret(c, n);
}

/* UINT waveOutPrepareHeader(HWAVEOUT, WAVEHDR FAR *, UINT) and Unprepare */
static void waveOutPrepareHeader(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    uint16_t s = FAR_SEL(a[1]), o = FAR_OFF(a[1]);
    if (!wave_get((uint16_t)a[0])) {
        c->a.x = MMSYSERR_INVALHANDLE;
    } else {
        wr32(s, o + WH_FLAGS, rd32(s, o + WH_FLAGS) | WHDR_PREPARED);
        c->a.x = 0;
    }
    api_ret(c, n);
}

static void waveOutUnprepareHeader(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    uint16_t s = FAR_SEL(a[1]), o = FAR_OFF(a[1]);
    uint32_t flags = rd32(s, o + WH_FLAGS);
    if (!wave_get((uint16_t)a[0])) {
        c->a.x = MMSYSERR_INVALHANDLE;
    } else if (flags & WHDR_INQUEUE) {
        c->a.x = WAVERR_STILLPLAYING;
    } else {
        wr32(s, o + WH_FLAGS, flags & ~(uint32_t)WHDR_PREPARED);
        c->a.x = 0;
    }
    api_ret(c, n);
}

/* UINT waveOutWrite(HWAVEOUT, WAVEHDR FAR *, UINT): the data is copied into the stream, so
 * the program may refill the buffer as soon as it's marked done. */
static void waveOutWrite(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    Wave *w = wave_get((uint16_t)a[0]);
    uint16_t s = FAR_SEL(a[1]), o = FAR_OFF(a[1]);
    uint32_t flags = rd32(s, o + WH_FLAGS);
    if (!w) {
        c->a.x = MMSYSERR_INVALHANDLE;
    } else if (!(flags & WHDR_PREPARED)) {
        c->a.x = WAVERR_UNPREPARED;
    } else if (w->nq == MAX_QUEUED) {
        c->a.x = MMSYSERR_NOTSUPPORTED;
    } else {
        uint32_t len = rd32(s, o + WH_LENGTH);
        const void *data = guest_ptr(rd32(s, o + WH_DATA));
        uint32_t loops = (flags & (WHDR_BEGINLOOP | WHDR_ENDLOOP)) == (WHDR_BEGINLOOP | WHDR_ENDLOOP)
                             ? rd32(s, o + WH_LOOPS) : 1;
        if (loops < 1 || loops > 64)
            loops = 1;
        for (uint32_t k = 0; k < loops && len; k++)
            SDL_PutAudioStreamData(w->stream, data, (int)len);
        static SDL_IOStream *dump;                /* COK_WAVE_DUMP=file.raw: debugging */
        static int dump_init;
        if (!dump_init) {
            dump_init = 1;
            const char *path = SDL_getenv("COK_WAVE_DUMP");
            dump = path ? SDL_IOFromFile(path, "wb") : NULL;
        }
        if (dump && len) {
            SDL_WriteIO(dump, data, len);
            SDL_FlushIO(dump);
        }
        w->written += (uint64_t)len * loops;
        wr32(s, o + WH_FLAGS, (flags & ~(uint32_t)WHDR_DONE) | WHDR_INQUEUE);
        w->q[w->nq].hdr = a[1];
        w->q[w->nq].end = w->written;
        w->nq++;
        c->a.x = 0;
    }
    api_ret(c, n);
}

static void waveOutPause(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wave *w = wave_get((uint16_t)a[0]);
    if (w) {
        SDL_PauseAudioStreamDevice(w->stream);
        w->paused = 1;
    }
    c->a.x = w ? 0 : MMSYSERR_INVALHANDLE;
    api_ret(c, n);
}

static void waveOutRestart(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wave *w = wave_get((uint16_t)a[0]);
    if (w) {
        SDL_ResumeAudioStreamDevice(w->stream);
        w->paused = 0;
    }
    c->a.x = w ? 0 : MMSYSERR_INVALHANDLE;
    api_ret(c, n);
}

/* Stop, drop everything queued and return all buffers as done. */
static void waveOutReset(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    Wave *w = wave_get((uint16_t)a[0]);
    if (w) {
        SDL_ClearAudioStream(w->stream);
        for (int i = 0; i < w->nq; i++)
            w->q[i].end = 0;
        wave_progress(w);
        w->written = 0;
    }
    c->a.x = w ? 0 : MMSYSERR_INVALHANDLE;
    api_ret(c, n);
}

/* UINT waveOutGetPosition(HWAVEOUT, MMTIME FAR *, UINT) */
static void waveOutGetPosition(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    Wave *w = wave_get((uint16_t)a[0]);
    uint16_t s = FAR_SEL(a[1]), o = FAR_OFF(a[1]);
    if (w) {
        int queued = SDL_GetAudioStreamQueued(w->stream);
        uint64_t played = w->written - (uint64_t)(queued > 0 ? queued : 0);
        uint16_t type = rd16(s, o);
        if (type == 1) {                          /* TIME_MS */
            wr32(s, o + 2, (uint32_t)(played * 1000 / (uint64_t)w->bytes_per_sec));
        } else if (type == 2) {                   /* TIME_SAMPLES */
            wr32(s, o + 2, (uint32_t)(played / (uint64_t)w->block_align));
        } else {                                  /* TIME_BYTES, and the fallback for others */
            wr16(s, o, 4);
            wr32(s, o + 2, (uint32_t)played);
        }
    }
    c->a.x = w ? 0 : MMSYSERR_INVALHANDLE;
    api_ret(c, n);
}

/* UINT waveOutGetVolume(UINT id, DWORD FAR *) / waveOutSetVolume(UINT id, DWORD) */
static void waveOutGetVolume(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    if (a[1])
        wr32(FAR_SEL(a[1]), FAR_OFF(a[1]), g_volume);
    c->a.x = 0;
    api_ret(c, n);
}

static void waveOutSetVolume(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    g_volume = a[1];
    for (int i = 0; i < MAX_WAVES; i++)
        if (g_wave[i].open)
            SDL_SetAudioStreamGain(g_wave[i].stream, wave_gain());
    c->a.x = 0;
    api_ret(c, n);
}

/* ---- multimedia timers ----------------------------------------------------------------- */

/* UINT timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK, DWORD user, UINT flags) */
static void timeSetEvent(CPU *c)
{
    uint32_t a[5];
    uint16_t n = api_args(c, "wwllw", a);
    int i = 0;
    while (i < MAX_TIMERS && g_timer[i].id)
        i++;
    if (i == MAX_TIMERS || !a[2]) {
        c->a.x = 0;
    } else {
        Timer *t = &g_timer[i];
        t->id = g_next_timer++;
        if (!g_next_timer)
            g_next_timer = 1;
        t->delay = a[0] ? a[0] : 1;
        t->due = rt_ticks() + t->delay;
        t->proc = a[2];
        t->user = a[3];
        t->periodic = (a[4] & 1) != 0;            /* TIME_PERIODIC */
        count_active();
        LOGI("timeSetEvent(%u ms, %s) = %u", t->delay, t->periodic ? "periodic" : "once", t->id);
        c->a.x = t->id;
    }
    api_ret(c, n);
}

static void timeKillEvent(CPU *c)      /* UINT timeKillEvent(UINT id) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 97;                                  /* TIMERR_NOCANDO */
    for (int i = 0; i < MAX_TIMERS; i++)
        if (a[0] && g_timer[i].id == a[0]) {
            g_timer[i].id = 0;
            c->a.x = 0;
        }
    count_active();
    api_ret(c, n);
}

const ApiDef g_wave_api[] = {
    { "MMSYSTEM", 401, "waveOutGetNumDevs", waveOutGetNumDevs },
    { "MMSYSTEM", 402, "waveOutGetDevCaps", waveOutGetDevCaps },
    { "MMSYSTEM", 404, "waveOutOpen", waveOutOpen },
    { "MMSYSTEM", 405, "waveOutClose", waveOutClose },
    { "MMSYSTEM", 406, "waveOutPrepareHeader", waveOutPrepareHeader },
    { "MMSYSTEM", 407, "waveOutUnprepareHeader", waveOutUnprepareHeader },
    { "MMSYSTEM", 408, "waveOutWrite", waveOutWrite },
    { "MMSYSTEM", 409, "waveOutPause", waveOutPause },
    { "MMSYSTEM", 410, "waveOutRestart", waveOutRestart },
    { "MMSYSTEM", 411, "waveOutReset", waveOutReset },
    { "MMSYSTEM", 412, "waveOutGetPosition", waveOutGetPosition },
    { "MMSYSTEM", 415, "waveOutGetVolume", waveOutGetVolume },
    { "MMSYSTEM", 416, "waveOutSetVolume", waveOutSetVolume },
    { "MMSYSTEM", 602, "timeSetEvent", timeSetEvent },
    { "MMSYSTEM", 603, "timeKillEvent", timeKillEvent },
    { NULL, 0, NULL, NULL }
};
