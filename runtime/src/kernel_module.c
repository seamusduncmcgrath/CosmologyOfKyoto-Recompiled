/* KERNEL module functions: handles, file names, GetProcAddress, LoadLibrary.
 *
 * Besides loaded NE modules, the program can see "system modules" (KERNEL, USER, ...),
 * which the runtime implements: they get small fake handles, and GetProcAddress on them
 * returns API thunk addresses. LoadLibrary of an optional DLL that wasn't on the original
 * disc (MACROMIX, WinG, QuickTime) fails as it would have in 1995, so Director takes the
 * same fallback paths.
 */
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "rt.h"

/* Standard Windows 3.1 modules the runtime stands in for. */
static const char *const g_system_modules[] = {
    "KERNEL", "USER", "GDI", "KEYBOARD", "SOUND", "SYSTEM", "DISPLAY", "MMSYSTEM", "SHELL",
    "COMMDLG", "WIN87EM", "TOOLHELP", "VER", "LZEXPAND", "MSVIDEO",
    "DIRDIB", "DIB",            /* DIB device drivers: GDI provides DIB devices natively */
    NULL
};
static uint16_t g_system_handle[32];

static void upper_base(const char *in, char *out, size_t cap)
{
    const char *p = in + strlen(in);
    while (p > in && p[-1] != '\\' && p[-1] != '/' && p[-1] != ':')
        p--;
    size_t i = 0;
    for (; p[i] && p[i] != '.' && i < cap - 1; i++)
        out[i] = (char)toupper((unsigned char)p[i]);
    out[i] = 0;
}

static int system_index(const char *name)
{
    for (int i = 0; g_system_modules[i]; i++)
        if (!strcmp(g_system_modules[i], name))
            return i;
    return -1;
}

static uint16_t system_handle(int i)
{
    if (!g_system_handle[i]) {
        /* a data segment with a local heap: programs measure USER's and GDI's free heap
         * space by locking the module handle and calling the local heap functions on it */
        g_system_handle[i] = sel_alloc(0x10000, SEL_DATA);
        memcpy(mem_ptr(g_system_handle[i], 0), "NE", 2);   /* looks like a module header */
        local_init(g_system_handle[i], 0x1000, 0xFFF0);
    }
    return g_system_handle[i];
}

static int system_by_handle(uint16_t h)
{
    for (int i = 0; g_system_modules[i]; i++)
        if (g_system_handle[i] && (g_system_handle[i] | 7) == (h | 7))
            return i;
    return -1;
}

static void GetModuleHandle(CPU *c)    /* HMODULE GetModuleHandle(LPCSTR) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t h = 0;
    if (FAR_SEL(a[0]) == 0) {                      /* an instance handle in the low word */
        LoadedModule *m = module_by_handle(FAR_OFF(a[0]));
        h = m ? m->hmodule : 0;
    } else {
        char name[16];
        upper_base(guest_ptr(a[0]), name, sizeof(name));
        LoadedModule *m = module_by_name(name);
        int si = system_index(name);
        h = m ? m->hmodule : si >= 0 ? system_handle(si) : 0;
        LOGD("GetModuleHandle(%s) = %04x", guest_ptr(a[0]), h);
    }
    c->a.x = h;
    c->d.x = 0;
    api_ret(c, n);
}

static void GetModuleFileName(CPU *c)  /* int GetModuleFileName(HINSTANCE, LPSTR, int) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    LoadedModule *m = module_by_handle((uint16_t)a[0]);
    int si = system_by_handle((uint16_t)a[0]);
    char path[160] = "";
    if (m)
        snprintf(path, sizeof(path), "%s", m->guest_path);
    else if (si >= 0)
        snprintf(path, sizeof(path), "C:\\WINDOWS\\SYSTEM\\%s.DLL", g_system_modules[si]);
    size_t len = strlen(path), cap = (uint16_t)a[2];
    if (cap) {
        if (len >= cap)
            len = cap - 1;
        memcpy(guest_ptr(a[1]), path, len);
        guest_ptr(a[1])[len] = 0;
    }
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

static void GetProcAddress(CPU *c)     /* FARPROC GetProcAddress(HMODULE, LPCSTR) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    uint16_t ordinal = FAR_SEL(a[1]) ? 0 : FAR_OFF(a[1]);
    const char *name = FAR_SEL(a[1]) ? guest_ptr(a[1]) : NULL;
    uint32_t addr = 0;
    LoadedModule *m = module_by_handle((uint16_t)a[0]);
    int si = system_by_handle((uint16_t)a[0]);
    if (m) {
        uint16_t ord = ordinal, sel, off;
        if ((!name || module_export_by_name(m, name, &ord)) && module_entry(m, ord, &sel, &off))
            addr = (uint32_t)sel << 16 | off;
    } else if (si >= 0) {
        uint16_t value;
        static uint16_t thhook;
        if (!name && api_constant(g_system_modules[si], ordinal, &value))
            addr = value;
        else if (name && si == 0 && !strcmp(name, "THHOOK")) {
            /* KERNEL's internal task/heap list heads (used by TOOLHELP). Only the selector
             * table is filled in: Director counts the selectors in use through it. */
            if (!thhook) {
                thhook = sel_alloc(64, SEL_DATA);
                uint16_t tab = sel_table();
                wr16(thhook, 0x00, tab);                  /* hGlobalHeap */
                wr16(thhook, 0x02, tab);                  /* pGlobalHeap */
                wr16(thhook, 0x16, (uint16_t)(SEL_COUNT * 4));   /* SelTableLen */
                wr32(thhook, 0x18, 0);                    /* SelTableStart, from pGlobalHeap */
            }
            addr = (uint32_t)thhook << 16;
        } else
            addr = api_slot_addr(api_slot(g_system_modules[si], ordinal, name, name));
    }
    if (name)
        LOGD("GetProcAddress(%04x, %s) = %04x:%04x", a[0], name, addr >> 16, addr & 0xFFFF);
    else
        LOGD("GetProcAddress(%04x, #%u) = %04x:%04x", a[0], ordinal, addr >> 16, addr & 0xFFFF);
    api_ret32(c, n, addr);
}

/* HANDLE GetCodeHandle(FARPROC): the code segment's handle (all segments are loaded). */
static void GetCodeHandle(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = FAR_SEL(a[0]);
    api_ret(c, n);
}

static void OutputDebugString(CPU *c)  /* void OutputDebugString(LPCSTR) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    LOGD("OutputDebugString: %s", a[0] ? (const char *)guest_ptr(a[0]) : "");
    api_ret(c, n);
}

/* Single-instance program: the prolog of exported functions already loads DS, so the
 * "instance thunk" can simply be the function itself. */
static void MakeProcInstance(CPU *c)   /* FARPROC MakeProcInstance(FARPROC, HINSTANCE) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    api_ret32(c, n, a[0]);
}

static void FreeProcInstance(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    api_ret(c, n);
}

/* Initialise a freshly loaded DLL: LibEntry gets DI = hInstance, DS = DGROUP, CX = heap
 * size, ES:SI = command line; returns AX != 0 on success. */
static int init_dll(CPU *c, LoadedModule *m)
{
    if (!m->cs)
        return 1;
    CPU saved = *c;
    uint16_t dgroup = m->autodata ? m->segsel[m->autodata] : 0;
    if (dgroup && m->heap)
        local_init(dgroup, m->segs[m->autodata].minalloc, (uint16_t)(m->segs[m->autodata].minalloc + m->heap));
    c->di.x = m->hmodule;
    c->ds = dgroup;
    c->c.x = m->heap;
    c->es = 0;
    c->si.x = 0;
    rt_callback(c, m->segsel[m->cs], m->ip);
    int ok = c->a.x != 0;
    saved.a.x = c->a.x;
    *c = saved;
    return ok;
}

static void LoadLibrary(CPU *c)        /* HINSTANCE LoadLibrary(LPCSTR): < 32 is an error code */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    char name[16];
    upper_base(guest_ptr(a[0]), name, sizeof(name));
    uint16_t h = 2;                                /* file not found */
    LoadedModule *m = module_by_name(name);
    if (m) {
        h = m->hmodule;
    } else if (system_index(name) >= 0) {
        h = system_handle(system_index(name));
    } else {
        for (const KnownDll *k = g_known_dlls; k->name; k++) {
            if (strcmp(k->name, name))
                continue;
            /* the program's folder, then C:\WINDOWS (where the projector puts MACROMIX.DLL) */
            char host[600];
            snprintf(host, sizeof(host), "%s/%s", g_game_dir, k->file);
            if (!SDL_GetPathInfo(host, NULL) && g_win_dir[0])
                snprintf(host, sizeof(host), "%s/%s", g_win_dir, k->file);
            m = ne_load(host, k->rm);
            if (m && init_dll(c, m))
                h = m->hmodule;
            break;
        }
    }
    LOGI("LoadLibrary(%s) = %04x%s", guest_ptr(a[0]), h, h < 32 ? " (not available)" : "");
    c->a.x = h;
    api_ret(c, n);
}

static void FreeLibrary(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret(c, n);
}

const ApiDef g_kernel_module_api[] = {
    { "KERNEL", 93, "GetCodeHandle", GetCodeHandle },
    { "KERNEL", 115, "OutputDebugString", OutputDebugString },
    { "KERNEL", 47, "GetModuleHandle", GetModuleHandle },
    { "KERNEL", 49, "GetModuleFileName", GetModuleFileName },
    { "KERNEL", 50, "GetProcAddress", GetProcAddress },
    { "KERNEL", 51, "MakeProcInstance", MakeProcInstance },
    { "KERNEL", 52, "FreeProcInstance", FreeProcInstance },
    { "KERNEL", 95, "LoadLibrary", LoadLibrary },
    { "KERNEL", 96, "FreeLibrary", FreeLibrary },
    { NULL, 0, NULL, NULL }
};
