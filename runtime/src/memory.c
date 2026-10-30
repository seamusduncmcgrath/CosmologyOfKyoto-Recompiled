/* Guest memory: a Windows 3.1 style local descriptor table.
 *
 * Every selector maps to host memory through g_seg_base[sel >> 3]. A block larger than
 * 64 KB gets consecutive selectors, each based 64 KB further into one contiguous host
 * allocation, so huge-pointer arithmetic (adding __AHINCR = 8 to the selector) and
 * accesses that run past a 64 KB boundary both land in the right place.
 */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

uint8_t *g_seg_base[SEL_COUNT];
Selector g_sel[SEL_COUNT];

/* Selector index 0 is the null selector; low indices are left unused like real Windows. */
#define FIRST_INDEX 2
#define SLACK 16          /* bytes past the end, so small overruns don't hit the host heap */

/* KERNEL's selector table, which THHOOK exposes (SelTableStart, SelTableLen): a dword per
 * selector, nonzero for selectors of allocated blocks. Director counts the nonzero entries
 * to learn how many selectors are in use, then adjusts that count as it allocates and frees
 * global blocks; with no table its count starts at 0, goes negative on the first frees and
 * reads as "out of selectors" (and "not enough memory" for every movie after the first few).
 * Allocated on first use, kept up to date from then on. */
static uint16_t g_seltab;

static void seltab_mark(unsigned i, int used)
{
    if (!g_seltab)
        return;
    uint32_t v = used ? (uint32_t)i << 3 | 7 : 0;
    memcpy(g_seg_base[SEL_INDEX(g_seltab)] + (size_t)i * 4, &v, 4);
}

uint16_t sel_table(void)
{
    if (!g_seltab) {
        uint16_t t = sel_alloc(SEL_COUNT * 4, SEL_DATA);
        g_seltab = t;
        for (unsigned i = FIRST_INDEX; i < SEL_COUNT; i++)
            if (g_sel[i].kind != SEL_FREE && !g_sel[i].alias)
                seltab_mark(i, 1);
    }
    return g_seltab;
}

static int find_free_run(unsigned count)
{
    unsigned run = 0;
    for (unsigned i = FIRST_INDEX; i < SEL_COUNT; i++) {
        run = g_sel[i].kind == SEL_FREE ? run + 1 : 0;
        if (run == count)
            return (int)(i - count + 1);
    }
    return -1;
}

uint16_t sel_alloc(uint32_t size, int kind)
{
    if (size == 0)
        size = 1;
    unsigned count = (size + 0xFFFF) >> 16;
    int first = find_free_run(count);
    if (first < 0) {
        LOGE("out of selectors (need %u)", count);
        return 0;
    }
    uint8_t *mem = calloc(1, (size_t)count * 0x10000 + SLACK);
    if (!mem) {
        LOGE("out of memory allocating %u bytes", size);
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        Selector *s = &g_sel[first + i];
        memset(s, 0, sizeof(*s));
        s->kind = (uint8_t)kind;
        s->alloc = mem;
        uint32_t remaining = size - i * 0x10000;
        s->limit = remaining >= 0x10000 ? 0xFFFF : remaining - 1;
        s->linear = (uint32_t)(first + i) << 16;
        g_seg_base[first + i] = mem + (size_t)i * 0x10000;
        seltab_mark(first + i, 1);
    }
    g_sel[first].count = (uint16_t)count;
    return SEL_MAKE(first);
}

void sel_free(uint16_t sel)
{
    unsigned i = SEL_INDEX(sel);
    if (i < FIRST_INDEX || i >= SEL_COUNT || g_sel[i].kind == SEL_FREE || !g_sel[i].count)
        return;
    unsigned count = g_sel[i].count;
    free(g_sel[i].alloc);
    for (unsigned k = 0; k < count; k++) {
        memset(&g_sel[i + k], 0, sizeof(Selector));
        g_seg_base[i + k] = NULL;
        seltab_mark(i + k, 0);
    }
}

/* Linear addresses: selector index i of a block owns [i << 16, (i + 1) << 16), so the
 * selectors of a huge block are contiguous in linear space as well. */
static uint8_t *linear_to_host(uint32_t lin)
{
    unsigned i = lin >> 16;
    if (i < FIRST_INDEX || i >= SEL_COUNT || !g_sel[i].alloc || g_sel[i].linear != (lin & 0xFFFF0000u))
        return NULL;
    return g_seg_base[i] + (lin & 0xFFFF);
}

uint16_t sel_alloc_alias(uint16_t src, int as_data)
{
    int i = find_free_run(1);
    if (i < 0)
        return 0;
    Selector *s = &g_sel[i];
    memset(s, 0, sizeof(*s));
    s->kind = SEL_DATA;
    if (sel_valid(src)) {
        const Selector *t = &g_sel[SEL_INDEX(src)];
        s->kind = as_data ? SEL_DATA : t->kind;
        s->limit = t->limit;
        s->linear = t->linear;
        g_seg_base[i] = g_seg_base[SEL_INDEX(src)];
    }
    s->alias = 1;
    return SEL_MAKE(i);
}

int sel_free_alias(uint16_t sel)
{
    unsigned i = SEL_INDEX(sel);
    if (!sel_valid(sel) || !g_sel[i].alias)
        return 0;
    memset(&g_sel[i], 0, sizeof(Selector));
    g_seg_base[i] = NULL;
    return 1;
}

int sel_set_base(uint16_t sel, uint32_t linear)
{
    unsigned i = SEL_INDEX(sel);
    uint8_t *host = linear_to_host(linear);
    if (!sel_valid(sel) || !g_sel[i].alias || !host)
        return 0;
    g_sel[i].linear = linear;
    g_seg_base[i] = host;
    return 1;
}

int sel_valid(uint16_t sel)
{
    unsigned i = SEL_INDEX(sel);
    return i >= FIRST_INDEX && i < SEL_COUNT && g_sel[i].kind != SEL_FREE;
}

uint32_t sel_size(uint16_t sel)
{
    unsigned i = SEL_INDEX(sel);
    if (!sel_valid(sel))
        return 0;
    uint32_t total = 0;
    while (i < SEL_COUNT && g_sel[i].kind != SEL_FREE) {
        total += g_sel[i].limit + 1;
        if (g_sel[i].limit != 0xFFFF)
            break;
        i++;
        if (i < SEL_COUNT && g_sel[i].count)      /* next block starts */
            break;
    }
    return total;
}
