#include <string.h>

#include "rt.h"

uint16_t api_args(CPU *c, const char *sig, uint32_t *out)
{
    size_t n = strlen(sig);
    uint16_t total = 0;
    for (size_t i = 0; i < n; i++)
        total += sig[i] == 'l' ? 4 : 2;
    /* the last parameter is nearest the return address */
    uint16_t off = (uint16_t)(c->sp.x + 4 + total);
    for (size_t i = 0; i < n; i++) {
        uint16_t size = sig[i] == 'l' ? 4 : 2;
        off -= size;
        out[i] = size == 4 ? rd32(c->ss, off) : rd16(c->ss, off);
    }
    return total;
}
