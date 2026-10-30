/* KERNEL (and WIN87EM) API functions: the task, strings, selectors, Catch/Throw. */
#include <string.h>

#include "rt.h"

uint16_t g_psp;
LoadedModule *g_task;
uint16_t g_cmd_show = 1;          /* SW_SHOWNORMAL */

#define WINFLAGS 0x0429           /* WF_PMODE | WF_CPU486 | WF_ENHANCED | WF_80x87 */

/* InitTask (KERNEL.91), register-based. Returns AX=1, CX=stack limit, DX=nCmdShow,
 * SI=hPrevInstance, DI=hInstance, ES:BX=command line in the PSP; fills the stack fields of
 * the instance data at DS:0 (pStackTop/pStackMin/pStackBottom). See Wine's InitTask16. */
static void InitTask(CPU *c)
{
    uint16_t ds = c->ds;
    uint16_t sp = (uint16_t)(c->sp.x + 4);        /* SP once we've returned */
    if (c->c.x) {                                   /* heap size: the local heap follows the stack */
        uint16_t data = g_task->segs[g_task->autodata].minalloc;
        local_init(ds, (uint16_t)(data + g_task->stack), (uint16_t)(data + g_task->stack + c->c.x));
    }
    uint16_t stacktop = (uint16_t)((sp > c->b.x ? sp - c->b.x : 0) + 150);
    wr16(ds, 0x0A, stacktop);                       /* pStackTop: lowest usable address */
    wr16(ds, 0x0C, sp);                             /* pStackMin */
    wr16(ds, 0x0E, sp);                             /* pStackBottom */
    const uint8_t *psp = mem_ptr(g_psp, 0);
    uint16_t bx = 0x81;
    while (bx < 0x81 + psp[0x80] && (psp[bx] == ' ' || psp[bx] == '\t'))
        bx++;
    c->a.x = 1;
    c->b.x = psp[0x80] ? bx : 0x80;
    c->c.x = stacktop;
    c->d.x = g_cmd_show;
    c->si.x = 0;
    c->di.x = ds;
    c->es = g_psp;
    api_ret(c, 0);
}

static void WaitEvent(CPU *c)          /* BOOL WaitEvent(HTASK) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void LockSegment(CPU *c)        /* HGLOBAL LockSegment(UINT seg); -1 = DS */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = a[0] == 0xFFFF ? c->ds : (uint16_t)a[0];
    api_ret(c, n);
}

static void UnlockSegment(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = a[0] == 0xFFFF ? c->ds : (uint16_t)a[0];
    api_ret(c, n);
}

static void GetVersion(CPU *c)         /* AX = Windows 3.10, DX = DOS 5.00 */
{
    c->a.x = 0x0A03;
    c->d.x = 0x0500;
    api_ret(c, 0);
}

static void GetWinFlags(CPU *c)
{
    api_ret32(c, 0, WINFLAGS);
}

static uint16_t g_error_mode;
static void SetErrorMode(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = g_error_mode;
    g_error_mode = (uint16_t)a[0];
    api_ret(c, n);
}

static void GetDOSEnvironment(CPU *c)  /* LPSTR GetDOSEnvironment(void): the block at PSP:2Ch */
{
    api_ret32(c, 0, (uint32_t)rd16(g_psp, 0x2C) << 16);
}

static void DOS3Call(CPU *c)           /* int 21h with the caller's registers */
{
    dos_int21(c);
    api_ret(c, 0);
}

static void FatalExit(CPU *c)
{
    uint32_t a[1];
    api_args(c, "w", a);
    rt_fatal("FatalExit(%d)", (int16_t)a[0]);
}

static void FatalAppExit(CPU *c)       /* void FatalAppExit(UINT, LPCSTR) */
{
    uint32_t a[2];
    api_args(c, "wl", a);
    const char *msg = guest_ptr(a[1]);
    rt_fatal("FatalAppExit: %s", msg ? msg : "(null)");
}

static void Yield(CPU *c)
{
    user_yield();
    api_ret(c, 0);
}

static void DirectedYield(CPU *c)      /* void DirectedYield(HTASK) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    user_yield();
    api_ret(c, n);
}

static void GetCurrentTask(CPU *c)     /* HTASK GetCurrentTask(void): DX = first task */
{
    c->a.x = g_task->hmodule;
    c->d.x = g_task->hmodule;
    api_ret(c, 0);
}

static void GetNumTasks(CPU *c)
{
    c->a.x = 1;
    api_ret(c, 0);
}

/* UINT WinExec(LPCSTR cmdline, UINT show): >= 32 on success. Nothing else runs here. */
static void WinExec(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    LOGW("WinExec(\"%s\") refused: no other programs in this runtime", guest_ptr(a[0]));
    c->a.x = 2;                                    /* file not found */
    api_ret(c, n);
}

/* ---- strings ---------------------------------------------------------------------------------- */

static void lstrcpy(CPU *c)            /* LPSTR lstrcpy(LPSTR, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    char *dst = guest_ptr(a[0]);
    const char *src = guest_ptr(a[1]);
    memmove(dst, src, strlen(src) + 1);
    api_ret32(c, n, a[0]);
}

static void lstrcat(CPU *c)            /* LPSTR lstrcat(LPSTR, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ll", a);
    char *dst = guest_ptr(a[0]);
    const char *src = guest_ptr(a[1]);
    memmove(dst + strlen(dst), src, strlen(src) + 1);
    api_ret32(c, n, a[0]);
}

static void lstrlen(CPU *c)            /* int lstrlen(LPCSTR) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    const char *s = guest_ptr(a[0]);
    c->a.x = s ? (uint16_t)strlen(s) : 0;
    api_ret(c, n);
}

/* ---- selectors ---------------------------------------------------------------------------------- */

static void AllocSelector(CPU *c)      /* UINT AllocSelector(UINT template) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = sel_alloc_alias((uint16_t)a[0], 0);
    api_ret(c, n);
}

static void AllocCStoDSAlias(CPU *c)   /* UINT AllocCStoDSAlias(UINT code selector) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = sel_alloc_alias((uint16_t)a[0], 1);
    api_ret(c, n);
}

static void FreeSelector(CPU *c)       /* UINT FreeSelector(UINT): 0 on success */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = sel_free_alias((uint16_t)a[0]) ? 0 : (uint16_t)a[0];
    api_ret(c, n);
}

static void GetSelectorBase(CPU *c)    /* DWORD GetSelectorBase(UINT) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret32(c, n, sel_valid((uint16_t)a[0]) ? g_sel[SEL_INDEX(a[0])].linear : 0);
}

static void SetSelectorBase(CPU *c)    /* UINT SetSelectorBase(UINT, DWORD): the selector, 0 on error */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    c->a.x = sel_set_base((uint16_t)a[0], a[1]) ? (uint16_t)a[0] : 0;
    api_ret(c, n);
}

static void SetSelectorLimit(CPU *c)   /* UINT SetSelectorLimit(UINT, DWORD) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    if (sel_valid((uint16_t)a[0]))
        g_sel[SEL_INDEX(a[0])].limit = a[1];
    c->a.x = 0;
    api_ret(c, n);
}

/* ---- Catch / Throw ------------------------------------------------------------------------------- *
 *
 * CATCHBUF (9 words, as Wine lays it out): IP, CS, SP after Catch returns, BP, SI, DI, DS,
 * unused, SS. The recompiled call site of Catch registers a host setjmp under the buffer's
 * address (see rt_jmp_slot); Throw restores the registers and host-longjmps there, which
 * resumes the guest just after its Catch call with AX = the thrown value.
 */
static void Catch(CPU *c)              /* int Catch(LPCATCHBUF) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    wr16(s, o + 0, rd16(c->ss, c->sp.x));
    wr16(s, o + 2, rd16(c->ss, (uint16_t)(c->sp.x + 2)));
    wr16(s, o + 4, (uint16_t)(c->sp.x + 4 + n));
    wr16(s, o + 6, c->bp.x);
    wr16(s, o + 8, c->si.x);
    wr16(s, o + 10, c->di.x);
    wr16(s, o + 12, c->ds);
    wr16(s, o + 14, 0);
    wr16(s, o + 16, c->ss);
    c->a.x = 0;
    api_ret(c, n);
}

static void Throw(CPU *c)              /* void Throw(const LPCATCHBUF, int) */
{
    uint32_t a[2];
    api_args(c, "lw", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    c->sp.x = rd16(s, o + 4);
    c->bp.x = rd16(s, o + 6);
    c->si.x = rd16(s, o + 8);
    c->di.x = rd16(s, o + 10);
    c->ds = rd16(s, o + 12);
    c->ss = rd16(s, o + 16);
    c->a.x = (uint16_t)a[1];
    LOGD("Throw(%04x:%04x, %d)", s, o, (int16_t)a[1]);
    rt_longjmp(c, a[0]);
}

/* ---- TOOLHELP ------------------------------------------------------------------------------- */

#define PAGE_SIZE 4096
#define TOTAL_PAGES ((32u << 20) / PAGE_SIZE)          /* a 32 MB machine */

static void MemManInfo(CPU *c)         /* BOOL MemManInfo(MEMMANINFO FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t s = FAR_SEL(a[0]), o = FAR_OFF(a[0]);
    uint32_t free_pages = TOTAL_PAGES * 3 / 4;
    wr32(s, o + 0x04, free_pages * PAGE_SIZE);         /* dwLargestFreeBlock */
    wr32(s, o + 0x08, free_pages);                     /* dwMaxPagesAvailable */
    wr32(s, o + 0x0C, free_pages);                     /* dwMaxPagesLockable */
    wr32(s, o + 0x10, TOTAL_PAGES * 2);                /* dwTotalLinearSpace */
    wr32(s, o + 0x14, free_pages);                     /* dwTotalUnlockedPages */
    wr32(s, o + 0x18, free_pages);                     /* dwFreePages */
    wr32(s, o + 0x1C, TOTAL_PAGES);                    /* dwTotalPages */
    wr32(s, o + 0x20, TOTAL_PAGES * 2 - TOTAL_PAGES / 4);  /* dwFreeLinearSpace */
    wr32(s, o + 0x24, 0);                              /* dwSwapFilePages */
    wr16(s, o + 0x28, PAGE_SIZE);                      /* wPageSize */
    c->a.x = 1;
    api_ret(c, n);
}

static void TimerCount(CPU *c)         /* BOOL TimerCount(TIMERINFO FAR *) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint32_t ms = rt_ticks();
    wr32(FAR_SEL(a[0]), FAR_OFF(a[0]) + 4, ms);
    wr32(FAR_SEL(a[0]), FAR_OFF(a[0]) + 8, ms);
    c->a.x = 1;
    api_ret(c, n);
}

static void LocalFirst(CPU *c)         /* BOOL LocalFirst(LOCALENTRY FAR *, HGLOBAL): no heap walking */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void LocalNext(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = 0;
    api_ret(c, n);
}

/* WIN87EM __fpMath: register-based, BX = function. Real FPU assumed. */
static void fpMath(CPU *c)
{
    switch (c->b.x) {
    case 0:                                          /* initialize */
    case 1:                                          /* reset */
        fpu_init(c);
        c->a.x = 0;
        break;
    case 4: c->fpu.cw = c->a.x; break;               /* load control word */
    case 5: c->a.x = c->fpu.cw; break;               /* store control word */
    case 6: c->a.x = fpu_status(c); c->fpu.sw &= 0xFF00; break;   /* get and clear status */
    case 8: c->a.x = 3; break;                       /* coprocessor type: 387 */
    default: break;                                  /* terminate, set handler, ... */
    }
    c->cf = 0;
    api_ret(c, 0);
}

const ApiDef g_kernel_api[] = {
    { "KERNEL", 1, "FatalExit", FatalExit },
    { "KERNEL", 3, "GetVersion", GetVersion },
    { "KERNEL", 23, "LockSegment", LockSegment },
    { "KERNEL", 24, "UnlockSegment", UnlockSegment },
    { "KERNEL", 29, "Yield", Yield },
    { "KERNEL", 30, "WaitEvent", WaitEvent },
    { "KERNEL", 36, "GetCurrentTask", GetCurrentTask },
    { "KERNEL", 55, "Catch", Catch },
    { "KERNEL", 56, "Throw", Throw },
    { "KERNEL", 88, "lstrcpy", lstrcpy },
    { "KERNEL", 89, "lstrcat", lstrcat },
    { "KERNEL", 90, "lstrlen", lstrlen },
    { "KERNEL", 91, "InitTask", InitTask },
    { "KERNEL", 102, "DOS3Call", DOS3Call },
    { "KERNEL", 107, "SetErrorMode", SetErrorMode },
    { "KERNEL", 131, "GetDOSEnvironment", GetDOSEnvironment },
    { "KERNEL", 132, "GetWinFlags", GetWinFlags },
    { "KERNEL", 137, "FatalAppExit", FatalAppExit },
    { "KERNEL", 150, "DirectedYield", DirectedYield },
    { "KERNEL", 152, "GetNumTasks", GetNumTasks },
    { "KERNEL", 166, "WinExec", WinExec },
    { "KERNEL", 170, "AllocCStoDSAlias", AllocCStoDSAlias },
    { "KERNEL", 175, "AllocSelector", AllocSelector },
    { "KERNEL", 176, "FreeSelector", FreeSelector },
    { "KERNEL", 186, "GetSelectorBase", GetSelectorBase },
    { "KERNEL", 187, "SetSelectorBase", SetSelectorBase },
    { "KERNEL", 189, "SetSelectorLimit", SetSelectorLimit },
    { "WIN87EM", 1, "__fpMath", fpMath },
    { "TOOLHELP", 0, "MemManInfo", MemManInfo },
    { "TOOLHELP", 0, "TimerCount", TimerCount },
    { "TOOLHELP", 0, "LocalFirst", LocalFirst },
    { "TOOLHELP", 0, "LocalNext", LocalNext },
    { NULL, 0, NULL, NULL }
};
