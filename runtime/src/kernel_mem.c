/* KERNEL memory management: the global heap (selector-based blocks) and local heaps.
 *
 * Global blocks are selector blocks from memory.c. Fixed blocks use the selector as their
 * handle; moveable blocks get handle = selector with bit 0 cleared, as Windows 3.1 does, and
 * GlobalLock turns either back into selector:0.
 *
 * Local heaps live inside a data segment (normally DGROUP, at its end after the stack). The
 * allocator's bookkeeping is kept on the host side; handles equal offsets (all blocks
 * behave as fixed, which is valid Windows behaviour).
 */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define FREE_SPACE (16u << 20)                     /* what we report as free: 16 MB */

static struct {
    uint16_t flags;
    uint8_t locks;
    uint32_t size;
} g_gblock[SEL_COUNT];

static uint16_t handle_to_sel(uint16_t h) { return (uint16_t)(h | 7); }

uint16_t global_sel(uint16_t h) { return handle_to_sel(h); }

uint16_t global_alloc(uint16_t flags, uint32_t size)
{
    uint16_t sel = sel_alloc(size ? size : 1, SEL_DATA);
    if (!sel)
        return 0;
    unsigned i = SEL_INDEX(sel);
    g_gblock[i].flags = flags;
    g_gblock[i].locks = 0;
    g_gblock[i].size = size;
    return (flags & GMEM_MOVEABLE) ? (uint16_t)(sel & ~1) : sel;
}

static int global_valid(uint16_t sel)
{
    return sel_valid(sel) && g_sel[SEL_INDEX(sel)].count && !g_sel[SEL_INDEX(sel)].module;
}

/* A local heap inside a global block grows the block, as Windows' local heap manager does
 * with GlobalReAlloc: programs read the new size back (GlobalSize) to bound their data. The
 * host memory behind a selector is always 64 KB, so only the bookkeeping changes. */
static void global_grow(uint16_t sel, uint32_t needed)
{
    if (!global_valid(sel))
        return;
    unsigned i = SEL_INDEX(sel);
    if (needed <= g_gblock[i].size || g_sel[i].count != 1)
        return;
    uint32_t size = (needed + 0x3FF) & ~0x3FFu;      /* grow in 1 KB steps */
    if (size > 0x10000)
        size = 0x10000;
    g_gblock[i].size = size;
    g_sel[i].limit = size - 1;
}

int global_free(uint16_t h)
{
    uint16_t sel = handle_to_sel(h);
    if (!global_valid(sel))
        return 0;
    sel_free(sel);
    g_gblock[SEL_INDEX(sel)].size = 0;
    return 1;
}

static void GlobalAlloc(CPU *c)        /* HGLOBAL GlobalAlloc(UINT flags, DWORD size) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "wl", a);
    c->a.x = global_alloc((uint16_t)a[0], a[1]);
    LOGD("GlobalAlloc(%04x, %u) = %04x", a[0], a[1], c->a.x);
    api_ret(c, n);
}

static void GlobalReAlloc(CPU *c)      /* HGLOBAL GlobalReAlloc(HGLOBAL, DWORD size, UINT flags) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    uint16_t sel = handle_to_sel((uint16_t)a[0]);
    c->a.x = 0;
    if (global_valid(sel)) {
        unsigned i = SEL_INDEX(sel);
        uint32_t old = g_gblock[i].size;
        uint32_t want = a[1] ? a[1] : 1;
        unsigned have = g_sel[i].count;
        if (((want + 0xFFFF) >> 16) <= have) {     /* fits the selectors we have: resize in place */
            for (unsigned k = 0; k < have; k++) {
                uint32_t rem = want > k * 0x10000u ? want - k * 0x10000u : 1;
                g_sel[i + k].limit = rem >= 0x10000 ? 0xFFFF : rem - 1;
            }
            if (want > old)
                memset(g_seg_base[i] + old, 0, want - old);
            g_gblock[i].size = a[1];
            c->a.x = (uint16_t)a[0];
        } else {                                   /* needs more selectors: move the block */
            uint16_t flags = g_gblock[i].flags;
            uint16_t h = global_alloc(flags, a[1]);
            if (h) {
                memcpy(g_seg_base[SEL_INDEX(handle_to_sel(h))], g_seg_base[i], old < want ? old : want);
                sel_free(sel);
                g_gblock[i].size = 0;
                c->a.x = h;
            }
        }
    }
    LOGD("GlobalReAlloc(%04x, %u, %04x) = %04x", a[0], a[1], a[2], c->a.x);
    api_ret(c, n);
}

static void GlobalFree(CPU *c)         /* HGLOBAL GlobalFree(HGLOBAL): NULL on success */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = global_free((uint16_t)a[0]) ? 0 : (uint16_t)a[0];
    api_ret(c, n);
}

static void GlobalLock(CPU *c)         /* void FAR *GlobalLock(HGLOBAL) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t sel = handle_to_sel((uint16_t)a[0]);
    if (a[0] && sel_valid(sel)) {
        if (g_gblock[SEL_INDEX(sel)].locks < 255)
            g_gblock[SEL_INDEX(sel)].locks++;
        api_ret32(c, n, (uint32_t)sel << 16);
    } else {
        api_ret32(c, n, 0);
    }
}

static void GlobalUnlock(CPU *c)       /* BOOL GlobalUnlock(HGLOBAL): nonzero while still locked */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t sel = handle_to_sel((uint16_t)a[0]);
    c->a.x = 0;
    if (sel_valid(sel) && g_gblock[SEL_INDEX(sel)].locks) {
        g_gblock[SEL_INDEX(sel)].locks--;
        c->a.x = g_gblock[SEL_INDEX(sel)].locks != 0;
    }
    api_ret(c, n);
}

static void GlobalSize(CPU *c)         /* DWORD GlobalSize(HGLOBAL) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t sel = handle_to_sel((uint16_t)a[0]);
    uint32_t size = 0;
    if (sel_valid(sel))
        size = global_valid(sel) ? g_gblock[SEL_INDEX(sel)].size : sel_size(sel);
    api_ret32(c, n, size);
}

static void GlobalHandle(CPU *c)       /* DWORD GlobalHandle(UINT sel): LOWORD handle, HIWORD selector */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t sel = handle_to_sel((uint16_t)a[0]);
    uint32_t r = 0;
    if (sel_valid(sel)) {
        uint16_t h = (g_gblock[SEL_INDEX(sel)].flags & GMEM_MOVEABLE) ? (uint16_t)(sel & ~1) : sel;
        r = (uint32_t)sel << 16 | h;
    }
    api_ret32(c, n, r);
}

static void GlobalFlags(CPU *c)        /* UINT GlobalFlags(HGLOBAL): lock count | flags */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    uint16_t sel = handle_to_sel((uint16_t)a[0]);
    c->a.x = sel_valid(sel)
        ? (uint16_t)((g_gblock[SEL_INDEX(sel)].flags & GMEM_DISCARDABLE) | g_gblock[SEL_INDEX(sel)].locks) : 0;
    api_ret(c, n);
}

static void GlobalCompact(CPU *c)      /* DWORD GlobalCompact(DWORD) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    api_ret32(c, n, FREE_SPACE);
}

static void GetFreeSpace(CPU *c)       /* DWORD GetFreeSpace(UINT) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret32(c, n, FREE_SPACE);
}

/* ---- local heap ---------------------------------------------------------------------------- */

typedef struct LBlock {
    uint16_t off, size;          /* size includes nothing extra: user bytes */
    uint8_t used;
} LBlock;

typedef struct LHeap {
    uint16_t sel;
    uint16_t start, end;         /* [start, end) inside the segment */
    LBlock *blocks;
    int n, cap;
} LHeap;

#define MAX_LHEAPS 64
static LHeap g_lheap[MAX_LHEAPS];

static LHeap *lheap_for(uint16_t sel)
{
    for (int i = 0; i < MAX_LHEAPS; i++)
        if (g_lheap[i].sel && SEL_INDEX(g_lheap[i].sel) == SEL_INDEX(sel))
            return &g_lheap[i];
    return NULL;
}

int local_init(uint16_t sel, uint16_t start, uint16_t end)
{
    LHeap *h = lheap_for(sel);
    for (int i = 0; !h && i < MAX_LHEAPS; i++)
        if (!g_lheap[i].sel)
            h = &g_lheap[i];
    if (!h)
        return 0;
    free(h->blocks);
    memset(h, 0, sizeof(*h));
    h->sel = sel;
    h->start = (uint16_t)((start + 3) & ~3);
    h->end = 0xFFF0;             /* the heap may grow to the end of the 64 KB segment */
    (void)end;
    g_sel[SEL_INDEX(sel)].limit = 0xFFFF;
    wr16(sel, 6, h->start);      /* pLocalHeap in the instance data */
    return 1;
}

static uint16_t local_alloc(LHeap *h, uint16_t size)
{
    size = (uint16_t)((size + 3) & ~3);
    if (!size)
        size = 4;
    /* first fit among freed blocks */
    for (int i = 0; i < h->n; i++) {
        LBlock *b = &h->blocks[i];
        if (!b->used && b->size >= size) {
            b->used = 1;
            memset(mem_ptr(h->sel, b->off), 0, b->size);
            return b->off;
        }
    }
    uint16_t top = h->n ? (uint16_t)(h->blocks[h->n - 1].off + h->blocks[h->n - 1].size) : h->start;
    if ((uint32_t)top + size > h->end)
        return 0;
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 64;
        h->blocks = realloc(h->blocks, (size_t)h->cap * sizeof(LBlock));
    }
    h->blocks[h->n++] = (LBlock){ top, size, 1 };
    memset(mem_ptr(h->sel, top), 0, size);
    global_grow(h->sel, (uint32_t)top + size);
    return top;
}

static LBlock *local_find(LHeap *h, uint16_t off)
{
    for (int i = 0; i < h->n; i++)
        if (h->blocks[i].off == off && h->blocks[i].used)
            return &h->blocks[i];
    return NULL;
}

static LHeap *current_lheap(CPU *c)
{
    LHeap *h = lheap_for(c->ds);
    if (!h)
        rt_fatal("local heap call with DS=%04x, which has no local heap", c->ds);
    return h;
}

static void LocalInit(CPU *c)          /* BOOL LocalInit(UINT seg, UINT start, UINT end) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    uint16_t sel = a[0] ? (uint16_t)a[0] : c->ds;
    c->a.x = (uint16_t)local_init(sel, (uint16_t)a[1], (uint16_t)a[2]);
    api_ret(c, n);
}

static void LocalAlloc(CPU *c)         /* HLOCAL LocalAlloc(UINT flags, UINT size) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    c->a.x = local_alloc(current_lheap(c), (uint16_t)a[1]);
    api_ret(c, n);
}

static void LocalReAlloc(CPU *c)       /* HLOCAL LocalReAlloc(HLOCAL, UINT size, UINT flags) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "www", a);
    LHeap *h = current_lheap(c);
    LBlock *b = local_find(h, (uint16_t)a[0]);
    c->a.x = 0;
    if (b) {
        if (a[1] <= b->size) {
            c->a.x = b->off;
        } else {
            uint16_t old = b->off, oldsize = b->size;
            uint16_t nb = local_alloc(h, (uint16_t)a[1]);
            if (nb) {
                memmove(mem_ptr(h->sel, nb), mem_ptr(h->sel, old), oldsize);
                local_find(h, old)->used = 0;
                c->a.x = nb;
            }
        }
    }
    api_ret(c, n);
}

static void LocalFree(CPU *c)          /* HLOCAL LocalFree(HLOCAL): NULL on success */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    LBlock *b = local_find(current_lheap(c), (uint16_t)a[0]);
    if (b)
        b->used = 0;
    c->a.x = b ? 0 : (uint16_t)a[0];
    api_ret(c, n);
}

static void LocalLock(CPU *c)          /* void NEAR *LocalLock(HLOCAL) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = (uint16_t)a[0];
    api_ret(c, n);
}

static void LocalUnlock(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void LocalSize(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    LBlock *b = local_find(current_lheap(c), (uint16_t)a[0]);
    c->a.x = b ? b->size : 0;
    api_ret(c, n);
}

static void LocalHandle(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = (uint16_t)a[0];
    api_ret(c, n);
}

/* DWORD GlobalDOSAlloc(DWORD): memory below 1 MB for real-mode code, as selector (low word)
 * and paragraph (high word). Nothing runs in real mode here, so any block will do; the
 * paragraph is a made-up value. */
static void GlobalDOSAlloc(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    uint16_t h = a[0] && a[0] <= 0x100000 ? global_alloc(GMEM_ZEROINIT, a[0]) : 0;
    uint16_t sel = h ? global_sel(h) : 0;
    api_ret32(c, n, sel ? (uint32_t)(0x1000 + SEL_INDEX(sel)) << 16 | sel : 0);
}

static void GlobalDOSFree(CPU *c)      /* UINT GlobalDOSFree(UINT selector): 0 on success */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = global_free((uint16_t)a[0]) ? 0 : (uint16_t)a[0];
    api_ret(c, n);
}

/* Page locking keeps memory resident for interrupt-time code; ours always is. */
static void GlobalPageLock(CPU *c) { uint32_t a[1]; uint16_t n = api_args(c, "w", a); c->a.x = a[0] ? 1 : 0; api_ret(c, n); }
static void GlobalPageUnlock(CPU *c) { uint32_t a[1]; uint16_t n = api_args(c, "w", a); c->a.x = 0; api_ret(c, n); }

const ApiDef g_kernel_mem_api[] = {
    { "KERNEL", 184, "GlobalDOSAlloc", GlobalDOSAlloc },
    { "KERNEL", 185, "GlobalDOSFree", GlobalDOSFree },
    { "KERNEL", 191, "GlobalPageLock", GlobalPageLock },
    { "KERNEL", 192, "GlobalPageUnlock", GlobalPageUnlock },
    { "KERNEL", 4, "LocalInit", LocalInit },
    { "KERNEL", 5, "LocalAlloc", LocalAlloc },
    { "KERNEL", 6, "LocalReAlloc", LocalReAlloc },
    { "KERNEL", 7, "LocalFree", LocalFree },
    { "KERNEL", 8, "LocalLock", LocalLock },
    { "KERNEL", 9, "LocalUnlock", LocalUnlock },
    { "KERNEL", 10, "LocalSize", LocalSize },
    { "KERNEL", 11, "LocalHandle", LocalHandle },
    { "KERNEL", 15, "GlobalAlloc", GlobalAlloc },
    { "KERNEL", 16, "GlobalReAlloc", GlobalReAlloc },
    { "KERNEL", 17, "GlobalFree", GlobalFree },
    { "KERNEL", 18, "GlobalLock", GlobalLock },
    { "KERNEL", 19, "GlobalUnlock", GlobalUnlock },
    { "KERNEL", 20, "GlobalSize", GlobalSize },
    { "KERNEL", 21, "GlobalHandle", GlobalHandle },
    { "KERNEL", 22, "GlobalFlags", GlobalFlags },
    { "KERNEL", 25, "GlobalCompact", GlobalCompact },
    { "KERNEL", 169, "GetFreeSpace", GetFreeSpace },
    { NULL, 0, NULL, NULL }
};
