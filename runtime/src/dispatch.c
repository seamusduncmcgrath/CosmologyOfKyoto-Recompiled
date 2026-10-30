/* Call dispatch: guest far/near addresses -> recompiled functions or runtime API thunks.
 *
 * Win16 API functions live in a "thunk segment": slot N is at g_thunk_sel:N*4. Imports
 * referenced by relocations or returned by GetProcAddress get these addresses, so calls
 * through stored function pointers reach the same implementation as direct calls.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define MAX_SLOTS 2048
#define CALLBACK_RETURN_OFF 0xFFF0

typedef struct ApiSlot {
    char module[16];
    uint16_t ordinal;
    char name[48];               /* imported name, or label for display */
    RecompFn fn;
} ApiSlot;

static ApiSlot g_slots[MAX_SLOTS];
static int g_nslots;
uint16_t g_thunk_sel;

extern const ApiDef *const g_api_tables[];   /* api_table.c */

static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* module names may be given as "mmsystem.dll" */
static void base_module(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in[i] && in[i] != '.' && i < cap - 1; i++)
        out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = 0;
}

static RecompFn find_impl(const char *module, uint16_t ordinal, const char *name, uint16_t *ord_out,
                          const char **name_out)
{
    for (const ApiDef *const *t = g_api_tables; *t; t++) {
        for (const ApiDef *d = *t; d->module; d++) {
            if (!name_eq(d->module, module))
                continue;
            if ((ordinal && d->ordinal == ordinal) || (name && d->name && name_eq(d->name, name))) {
                *ord_out = d->ordinal;
                *name_out = d->name;
                return d->fn;
            }
        }
    }
    return NULL;
}

int api_slot(const char *module_in, uint16_t ordinal, const char *name, const char *label)
{
    char module[16];
    base_module(module_in, module, sizeof(module));
    uint16_t ord = ordinal;
    const char *impl_name = NULL;
    RecompFn fn = find_impl(module, ordinal, name, &ord, &impl_name);
    for (int i = 1; i < g_nslots; i++) {
        ApiSlot *s = &g_slots[i];
        if (!name_eq(s->module, module))
            continue;
        if ((ord && s->ordinal == ord) || (name && name_eq(s->name, name))) {
            if (!s->name[0] && label)              /* created by a relocation, before a label was known */
                snprintf(s->name, sizeof(s->name), "%s", label);
            return i;
        }
    }
    if (g_nslots == 0)
        g_nslots = 1;                              /* slot 0: callback return marker */
    if (g_nslots == MAX_SLOTS)
        rt_fatal("too many API slots");
    ApiSlot *s = &g_slots[g_nslots];
    snprintf(s->module, sizeof(s->module), "%s", module);
    s->ordinal = ord;
    snprintf(s->name, sizeof(s->name), "%s", impl_name ? impl_name : name ? name : label ? label : "");
    s->fn = fn;
    return g_nslots++;
}

uint32_t api_slot_addr(int slot)
{
    if (!g_thunk_sel) {
        g_thunk_sel = sel_alloc(MAX_SLOTS * 4, SEL_CODE);
        memset(g_seg_base[SEL_INDEX(g_thunk_sel)], 0xCC, MAX_SLOTS * 4);
    }
    return (uint32_t)g_thunk_sel << 16 | (uint32_t)(slot * 4);
}

int api_constant(const char *module_in, uint16_t ordinal, uint16_t *value)
{
    char module[16];
    base_module(module_in, module, sizeof(module));
    if (strcmp(module, "KERNEL") != 0)
        return 0;
    switch (ordinal) {
    case 113: *value = 3; return 1;               /* __AHSHIFT */
    case 114: *value = 8; return 1;               /* __AHINCR */
    case 178: *value = 0x0429; return 1;          /* __WINFLAGS: pmode, 486, enhanced, 80x87 */
    }
    return 0;
}

static const char *caller(CPU *c)
{
    /* the return address the call pushed: far for API calls; for a near call only the IP
     * is meaningful, so show both readings */
    static char buf[128];
    uint16_t ip = rd16(c->ss, c->sp.x), cs = rd16(c->ss, (uint16_t)(c->sp.x + 2));
    if (sel_valid(cs) && g_sel[SEL_INDEX(cs)].module)
        return rt_where(cs, ip);
    snprintf(buf, sizeof(buf), "near return IP %04x", ip);
    return buf;
}

/* Walk the guest BP chain (Win16 frames: [bp] = caller's BP, odd for far frames, then the
 * return IP and, for far frames, CS) and log each return address. */
void rt_backtrace(CPU *c)
{
    uint16_t bp = c->bp.x;
    LOGI("  called from %s", caller(c));
    for (int depth = 0; depth < 24 && bp && bp < 0xFFF8; depth++) {
        uint16_t saved = rd16(c->ss, bp), ip = rd16(c->ss, (uint16_t)(bp + 2)), cs = rd16(c->ss, (uint16_t)(bp + 4));
        /* MSVC far frames don't always mark BP odd; trust a CS that is a code segment */
        if ((saved & 1) || (sel_valid(cs) && g_sel[SEL_INDEX(cs)].module))
            LOGI("  #%d %s  (callee args %04x %04x %04x %04x)", depth, rt_where(cs, ip), rd16(c->ss, (uint16_t)(bp + 6)),
                 rd16(c->ss, (uint16_t)(bp + 8)), rd16(c->ss, (uint16_t)(bp + 10)), rd16(c->ss, (uint16_t)(bp + 12)));
        else
            LOGI("  #%d near %04x", depth, ip);
        uint16_t next = saved & ~1;
        if (next <= bp)
            break;
        bp = next;
    }
}

void api_call_slot(CPU *c, int slot)
{
    if (slot <= 0 || slot >= g_nslots)
        rt_fatal("call to invalid API slot %d", slot);
    ApiSlot *s = &g_slots[slot];
    static const char *trace;
    static int trace_init;
    if (!trace_init) {
        trace_init = 1;
        trace = getenv("COK_BACKTRACE");
    }
    static long trace_hits, trace_every = -1;
    if (trace_every < 0) {
        const char *e = getenv("COK_DUMP_EVERY");     /* only every Nth hit (long-running loops) */
        trace_every = e ? atol(e) : 1;
        if (trace_every < 1)
            trace_every = 1;
    }
    if (trace && name_eq(trace, s->name) && trace_hits++ % trace_every == 0) {
        LOGI("backtrace at %s.%s (hit %ld, %u ms):", s->module, s->name, trace_hits, rt_ticks());
        if (trace_every == 1)
            rt_backtrace(c);
        /* COK_DUMP=off,len[;off,len...]: hex dump of DGROUP (the task's data segment) at that
         * point; "*ptr+off,len" dumps through a far pointer stored at DGROUP:ptr */
        for (const char *dump = getenv("COK_DUMP"); dump && *dump; dump = strchr(dump, ';') ? strchr(dump, ';') + 1 : "") {
            unsigned off = 0, len = 0, ptr = 0;
            uint16_t ds = g_task->segsel[g_task->autodata];
            if (dump[0] == '*' && sscanf(dump + 1, "%x+%x,%x", &ptr, &off, &len) == 3) {
                uint32_t fp = rd32(ds, ptr);
                ds = FAR_SEL(fp);
                off += FAR_OFF(fp);
            } else {
                sscanf(dump, "%x,%x", &off, &len);
            }
            for (unsigned i = 0; i < len; i += 16) {
                char line[80];
                int p = snprintf(line, sizeof(line), "  %04x:", off + i);
                for (unsigned j = 0; j < 16 && i + j < len; j++)
                    p += snprintf(line + p, sizeof(line) - p, " %02x", rd8(ds, off + i + j));
                LOGI("%s", line);
            }
        }
    }
    if (!s->fn)
        rt_fatal("unimplemented API %s.%u %s, called from %s", s->module, s->ordinal, s->name, caller(c));
    /* Pascal arguments are pushed left to right: the last ones are nearest SP */
    LOGD("api %s.%s from %s [%04x %04x %04x %04x %04x %04x]", s->module, s->name, caller(c),
         rd16(c->ss, (uint16_t)(c->sp.x + 4)), rd16(c->ss, (uint16_t)(c->sp.x + 6)), rd16(c->ss, (uint16_t)(c->sp.x + 8)),
         rd16(c->ss, (uint16_t)(c->sp.x + 10)), rd16(c->ss, (uint16_t)(c->sp.x + 12)), rd16(c->ss, (uint16_t)(c->sp.x + 14)));
    s->fn(c);
    if (g_dib_shadows_pending)
        gdi_flush_dib_shadows();
    mm_poll();                    /* interrupt-time callbacks run between API calls */
}

void rt_import(CPU *c, const RecompModule *mod, uint32_t index)
{
    uint16_t slot = mod->import_slot[index];
    if (slot != 0xFFFF) {
        api_call_slot(c, slot);
        return;
    }
    uint32_t addr = mod->import_addr[index];       /* export of another loaded module */
    rt_call_far(c, (uint16_t)(addr >> 16), (uint16_t)addr);
}

static RecompFn lookup(uint16_t sel, uint16_t off, LoadedModule **mod_out)
{
    if (!sel_valid(sel))
        return NULL;
    Selector *s = &g_sel[SEL_INDEX(sel)];
    LoadedModule *m = s->module;
    *mod_out = m;
    if (!m || !m->rm)
        return NULL;
    const RecompFunc *f = m->rm->funcs;
    uint32_t lo = 0, hi = m->rm->nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (f[mid].seg < s->segnum || (f[mid].seg == s->segnum && f[mid].off < off))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < m->rm->nfuncs && f[lo].seg == s->segnum && f[lo].off == off)
        return f[lo].fn;
    return NULL;
}

void rt_call_far(CPU *c, uint16_t sel, uint16_t off)
{
    if (sel == g_thunk_sel && g_thunk_sel) {
        api_call_slot(c, off / 4);
        return;
    }
    LoadedModule *m = NULL;
    RecompFn fn = lookup(sel, off, &m);
    /* a pointer to alignment NOPs just before a function: they do nothing, skip them */
    for (uint16_t o = off; !fn && m && o - off < 16 && rd8(sel, o) == 0x90; )
        fn = lookup(sel, ++o, &m);
    if (!fn)
        rt_fatal("call to %s, which isn't a recompiled function entry (from %s)", rt_where(sel, off), caller(c));
    fn(c);
}

void rt_call_near(CPU *c, uint16_t sel, uint16_t off)
{
    rt_call_far(c, sel, off);
}

void rt_callback(CPU *c, uint16_t sel, uint16_t off)
{
    push16(c, g_thunk_sel);
    push16(c, CALLBACK_RETURN_OFF);
    rt_call_far(c, sel, off);
}

const char *rt_where(uint16_t sel, uint16_t off)
{
    static char buf[4][96];
    static int n;
    char *b = buf[n++ & 3];
    if (sel == g_thunk_sel && g_thunk_sel) {
        int slot = off / 4;
        if (off == CALLBACK_RETURN_OFF)
            snprintf(b, 96, "runtime callback");
        else if (slot > 0 && slot < g_nslots)
            snprintf(b, 96, "thunk %s.%s", g_slots[slot].module, g_slots[slot].name);
        else
            snprintf(b, 96, "thunk %04x", off);
    } else if (sel_valid(sel) && g_sel[SEL_INDEX(sel)].module) {
        Selector *s = &g_sel[SEL_INDEX(sel)];
        snprintf(b, 96, "%s:%03u:%04x", s->module->name, s->segnum, off);
    } else {
        snprintf(b, 96, "%04x:%04x", sel, off);
    }
    return b;
}
