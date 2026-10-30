/* USER and GDI object handles. Win16 handles are 16-bit; here they index one table of
 * typed host objects. Values are multiples of 4 from 0x0100 up, like local-heap handles. */
#include "win.h"

#define MAX_HANDLES 16384
#define FIRST 0x40

static struct {
    uint8_t type;
    void *obj;
} g_h[MAX_HANDLES];
static int g_hint = FIRST;

uint16_t h_alloc(int type, void *obj)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = pass ? FIRST : g_hint; i < MAX_HANDLES; i++) {
            if (g_h[i].type == HT_FREE) {
                g_h[i].type = (uint8_t)type;
                g_h[i].obj = obj;
                g_hint = i + 1;
                return (uint16_t)(i << 2);
            }
        }
    }
    rt_fatal("out of USER/GDI handles");
}

static int index_of(uint16_t h)
{
    int i = h >> 2;
    return (h & 3) == 0 && i >= FIRST && i < MAX_HANDLES ? i : 0;
}

void *h_get(uint16_t h, int type)
{
    int i = index_of(h);
    return i && g_h[i].type == type ? g_h[i].obj : NULL;
}

int h_type(uint16_t h)
{
    int i = index_of(h);
    return i ? g_h[i].type : HT_FREE;
}

void h_free(uint16_t h)
{
    int i = index_of(h);
    if (i) {
        g_h[i].type = HT_FREE;
        g_h[i].obj = NULL;
        if (i < g_hint)
            g_hint = i;
    }
}
