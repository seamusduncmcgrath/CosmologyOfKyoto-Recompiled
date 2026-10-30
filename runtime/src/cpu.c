/* CPU-level services for recompiled code: interrupts, selector instructions, traps,
 * setjmp/longjmp slots. */
#include <string.h>

#include "rt.h"

void cpu_reset(CPU *c)
{
    memset(c, 0, sizeof(*c));
    c->ifl = 1;
    fpu_init(c);
}

void rt_int(CPU *c, uint8_t n)
{
    if (n == 0x21) {
        dos_int21(c);
        return;
    }
    if (n == 0x3D)              /* FWAIT through the FP emulator (an OS fixup site): no-op */
        return;
    rt_fatal("unimplemented software interrupt %02xh (AX=%04x)", n, c->a.x);
}

/* access rights as LAR reports them: present, DPL 3, code (exec/read) or data (read/write) */
static uint16_t access_rights(uint16_t sel)
{
    return g_sel[SEL_INDEX(sel)].kind == SEL_CODE ? 0xFA00 : 0xF200;
}

void rt_lar(CPU *c, uint16_t *dst, uint16_t sel)
{
    c->zf = (uint8_t)sel_valid(sel);
    if (c->zf)
        *dst = access_rights(sel);
}

void rt_lsl(CPU *c, uint16_t *dst, uint16_t sel)
{
    c->zf = (uint8_t)sel_valid(sel);
    if (c->zf)
        *dst = (uint16_t)g_sel[SEL_INDEX(sel)].limit;
}

void rt_verr(CPU *c, uint16_t sel)
{
    c->zf = (uint8_t)sel_valid(sel);
}

void rt_verw(CPU *c, uint16_t sel)
{
    c->zf = (uint8_t)(sel_valid(sel) && g_sel[SEL_INDEX(sel)].kind == SEL_DATA);
}

void rt_trap(CPU *c, const char *what, uint16_t sel, uint16_t off)
{
    (void)c;
    rt_fatal("%s at %s", what, rt_where(sel, off));
}

void rt_divide_error(CPU *c, uint16_t sel, uint16_t off)
{
    (void)c;
    rt_fatal("divide error at %s", rt_where(sel, off));
}

/* ---- setjmp / longjmp ------------------------------------------------------------------- */

#define MAX_JMP_SLOTS 64

static struct {
    uint32_t key;
    int used;
    jmp_buf jb;
} g_jmp[MAX_JMP_SLOTS];

jmp_buf *rt_jmp_slot(CPU *c, uint32_t key)
{
    (void)c;
    int free_slot = -1;
    for (int i = 0; i < MAX_JMP_SLOTS; i++) {
        if (g_jmp[i].used && g_jmp[i].key == key)
            return &g_jmp[i].jb;
        if (!g_jmp[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        rt_fatal("too many setjmp buffers");
    g_jmp[free_slot].used = 1;
    g_jmp[free_slot].key = key;
    return &g_jmp[free_slot].jb;
}

void rt_longjmp(CPU *c, uint32_t key)
{
    (void)c;
    for (int i = 0; i < MAX_JMP_SLOTS; i++)
        if (g_jmp[i].used && g_jmp[i].key == key)
            longjmp(g_jmp[i].jb, 1);
    rt_fatal("longjmp to %04x:%04x, which no setjmp registered", key >> 16, key & 0xFFFF);
}
