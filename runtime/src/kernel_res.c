/* NE resources: lookup in a module's resource table, and the KERNEL resource API.
 *
 * Resource data is read straight from the module file (the self-loader only packs code
 * and data segments). LoadResource copies a resource into a global block once and hands
 * out the same handle while it stays loaded, as Windows does.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

ResId res_id_guest(uint32_t far)
{
    ResId r = { 0, NULL };
    if (FAR_SEL(far) == 0) {
        r.id = FAR_OFF(far);
    } else {
        r.name = guest_ptr(far);
        if (r.name[0] == '#') {
            r.id = (uint16_t)strtol(r.name + 1, NULL, 10);
            r.name = NULL;
        }
    }
    return r;
}

static int pstr_ieq(const uint8_t *p, const char *s)
{
    size_t n = p[0];
    if (strlen(s) != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (toupper(p[1 + i]) != toupper((unsigned char)s[i]))
            return 0;
    return 1;
}

/* Does a type or name field of the table (high bit = integer id, else offset of a Pascal
 * string from the table start) match? */
static int res_match(const uint8_t *table, uint16_t field, ResId want)
{
    if (field & 0x8000)
        return want.name == NULL && (field & 0x7FFF) == want.id;
    return want.name != NULL && pstr_ieq(table + field, want.name);
}

const uint8_t *res_find(LoadedModule *m, ResId type, ResId name, uint32_t *size)
{
    const uint8_t *ne = m->file + m->ne;
    uint16_t rtab_off = u16(ne + 0x24);
    if (rtab_off == u16(ne + 0x26))               /* empty resource table */
        return NULL;
    const uint8_t *table = ne + rtab_off;
    uint16_t shift = u16(table);
    const uint8_t *p = table + 2;
    while (u16(p)) {
        uint16_t tid = u16(p), count = u16(p + 2);
        const uint8_t *e = p + 8;
        if (res_match(table, tid, type)) {
            for (uint16_t i = 0; i < count; i++, e += 12) {
                if (res_match(table, u16(e + 6), name)) {
                    uint32_t off = (uint32_t)u16(e) << shift, len = (uint32_t)u16(e + 2) << shift;
                    if (off + len > m->file_size)
                        return NULL;
                    *size = len;
                    return m->file + off;
                }
            }
        }
        p += 8 + 12 * count;
    }
    return NULL;
}

/* ---- the KERNEL API ---------------------------------------------------------------------------- */

typedef struct HRsrc {
    LoadedModule *m;
    const uint8_t *data;
    uint32_t size;
    uint16_t hglobal;
    uint16_t refs;
} HRsrc;

#define MAX_HRSRC 512
static HRsrc g_hrsrc[MAX_HRSRC];
static int g_nhrsrc;

static void describe(ResId r, char *out, size_t cap)
{
    if (r.name)
        snprintf(out, cap, "\"%s\"", r.name);
    else
        snprintf(out, cap, "#%u", r.id);
}

static void FindResource(CPU *c)       /* HRSRC FindResource(HINSTANCE, LPCSTR name, LPCSTR type) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wll", a);
    LoadedModule *m = module_by_handle((uint16_t)a[0]);
    ResId name = res_id_guest(a[1]), type = res_id_guest(a[2]);
    uint32_t size = 0;
    const uint8_t *data = m ? res_find(m, type, name, &size) : NULL;
    uint16_t h = 0;
    if (data) {
        for (int i = 0; i < g_nhrsrc; i++)
            if (g_hrsrc[i].data == data)
                h = (uint16_t)(i + 1);
        if (!h && g_nhrsrc < MAX_HRSRC) {
            g_hrsrc[g_nhrsrc] = (HRsrc){ m, data, size, 0, 0 };
            h = (uint16_t)++g_nhrsrc;
        }
    }
    char tn[64], nn[64];
    describe(type, tn, sizeof(tn));
    describe(name, nn, sizeof(nn));
    LOGD("FindResource(%04x, %s, type %s) = %04x", a[0], nn, tn, h);
    c->a.x = h;
    api_ret(c, n);
}

static HRsrc *hrsrc(uint16_t h)
{
    return h >= 1 && h <= g_nhrsrc ? &g_hrsrc[h - 1] : NULL;
}

static void LoadResource(CPU *c)       /* HGLOBAL LoadResource(HINSTANCE, HRSRC) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    HRsrc *r = hrsrc((uint16_t)a[1]);
    c->a.x = 0;
    if (r) {
        if (!r->hglobal) {
            r->hglobal = global_alloc(GMEM_MOVEABLE, r->size);
            if (r->hglobal)
                memcpy(mem_ptr(global_sel(r->hglobal), 0), r->data, r->size);
        }
        if (r->hglobal)
            r->refs++;
        c->a.x = r->hglobal;
    }
    api_ret(c, n);
}

static void LockResource(CPU *c)       /* void FAR *LockResource(HGLOBAL) */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    api_ret32(c, n, a[0] ? (uint32_t)global_sel((uint16_t)a[0]) << 16 : 0);
}

static void FreeResource(CPU *c)       /* BOOL FreeResource(HGLOBAL): FALSE on success */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    for (int i = 0; i < g_nhrsrc; i++) {
        HRsrc *r = &g_hrsrc[i];
        if (r->hglobal && r->hglobal == (uint16_t)a[0] && r->refs && !--r->refs) {
            global_free(r->hglobal);
            r->hglobal = 0;
        }
    }
    c->a.x = 0;
    api_ret(c, n);
}

static void SizeofResource(CPU *c)     /* DWORD SizeofResource(HINSTANCE, HRSRC) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "ww", a);
    HRsrc *r = hrsrc((uint16_t)a[1]);
    api_ret32(c, n, r ? r->size : 0);
}

const ApiDef g_kernel_res_api[] = {
    { "KERNEL", 60, "FindResource", FindResource },
    { "KERNEL", 61, "LoadResource", LoadResource },
    { "KERNEL", 62, "LockResource", LockResource },
    { "KERNEL", 63, "FreeResource", FreeResource },
    { "KERNEL", 65, "SizeofResource", SizeofResource },
    { NULL, 0, NULL, NULL }
};
