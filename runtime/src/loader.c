/* Win16 NE module loader.
 *
 * Maps every segment (code and data) into guest memory, zero-filled to minalloc, and
 * applies the relocation records exactly as KERNEL would, including in code segments,
 * because recompiled code still reads tables and constants from CS. Imports resolve to
 * other loaded modules' entry points or to runtime API thunk addresses. Finally the
 * module's generated tables (selectors per segment, import slots and addresses) are filled.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define MAX_MODULES 16
static LoadedModule *g_modules[MAX_MODULES];
static int g_nmodules;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }

static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}

static void pstr(const uint8_t *p, char *out, size_t cap)
{
    size_t n = p[0] < cap - 1 ? p[0] : cap - 1;
    memcpy(out, p + 1, n);
    out[n] = 0;
}

LoadedModule *module_by_name(const char *name)
{
    for (int i = 0; i < g_nmodules; i++)
        if (name_eq(g_modules[i]->name, name))
            return g_modules[i];
    return NULL;
}

LoadedModule *module_by_handle(uint16_t h)
{
    for (int i = 0; i < g_nmodules; i++)
        if (g_modules[i]->hmodule == h)
            return g_modules[i];
    uint16_t sel = (uint16_t)(h | 7);
    if (sel_valid(sel) && g_sel[SEL_INDEX(sel)].module)
        return g_sel[SEL_INDEX(sel)].module;
    return NULL;
}

int module_entry(LoadedModule *m, uint16_t ordinal, uint16_t *sel, uint16_t *off)
{
    if (ordinal == 0 || ordinal >= m->nentries || m->entries[ordinal].seg == 0)
        return 0;
    uint16_t seg = m->entries[ordinal].seg;
    *off = m->entries[ordinal].off;
    *sel = seg == 0xFE ? 0 : m->segsel[seg];      /* 0xFE: absolute constant */
    return 1;
}

static int search_names(const uint8_t *p, const char *name, uint16_t *ordinal)
{
    p += 1 + p[0] + 2;                             /* skip the module name entry */
    while (p[0]) {
        char buf[256];
        pstr(p, buf, sizeof(buf));
        if (name_eq(buf, name)) {
            *ordinal = u16(p + 1 + p[0]);
            return 1;
        }
        p += 1 + p[0] + 2;
    }
    return 0;
}

int module_export_by_name(LoadedModule *m, const char *name, uint16_t *ordinal)
{
    const uint8_t *ne = m->file + m->ne;
    if (search_names(ne + u16(ne + 0x26), name, ordinal))
        return 1;
    uint32_t nonres = u32(ne + 0x2C);
    return nonres && nonres < m->file_size && search_names(m->file + nonres, name, ordinal);
}

static void parse_entries(LoadedModule *m)
{
    const uint8_t *ne = m->file + m->ne;
    const uint8_t *p = ne + u16(ne + 0x04), *end = p + u16(ne + 0x06);
    /* first pass: count ordinals */
    unsigned count = 1;
    for (const uint8_t *q = p; q < end && q[0];) {
        unsigned n = q[0], kind = q[1];
        count += n;
        q += 2 + (kind == 0 ? 0 : kind == 0xFF ? 6u * n : 3u * n);
    }
    m->nentries = (uint16_t)count;
    m->entries = calloc(count, sizeof(*m->entries));
    unsigned ord = 1;
    while (p < end && p[0]) {
        unsigned n = p[0], kind = p[1];
        p += 2;
        for (unsigned i = 0; i < n; i++, ord++) {
            if (kind == 0)
                continue;
            if (kind == 0xFF) {                    /* movable: flags, int 3Fh, seg, offset */
                m->entries[ord].seg = p[3];
                m->entries[ord].off = u16(p + 4);
                p += 6;
            } else {                               /* fixed: flags, offset */
                m->entries[ord].seg = (uint16_t)kind;
                m->entries[ord].off = u16(p + 1);
                p += 3;
            }
        }
    }
}

/* Resolve an imported function to a far address (or a constant offset). */
static void resolve_import(LoadedModule *m, unsigned modidx, uint16_t ordinal, const char *name,
                           uint16_t *sel, uint16_t *off)
{
    const uint8_t *ne = m->file + m->ne;
    char modname[16];
    pstr(ne + u16(ne + 0x2A) + u16(ne + u16(ne + 0x28) + 2 * (modidx - 1)), modname, sizeof(modname));

    LoadedModule *target = module_by_name(modname);
    if (target) {
        uint16_t ord = ordinal;
        if (name && !module_export_by_name(target, name, &ord))
            rt_fatal("%s imports %s.%s, which doesn't exist", m->name, modname, name);
        if (!module_entry(target, ord, sel, off))
            rt_fatal("%s imports %s.%u, which doesn't exist", m->name, modname, ord);
        return;
    }
    uint16_t value;
    if (!name && api_constant(modname, ordinal, &value)) {
        *sel = 0;
        *off = value;
        return;
    }
    uint32_t addr = api_slot_addr(api_slot(modname, ordinal, name, NULL));
    *sel = (uint16_t)(addr >> 16);
    *off = (uint16_t)addr;
}

static void apply_relocs(LoadedModule *m, unsigned segnum, const uint8_t *rel)
{
    const uint8_t *ne = m->file + m->ne;
    uint8_t *mem = g_seg_base[SEL_INDEX(m->segsel[segnum])];
    uint32_t limit = g_sel[SEL_INDEX(m->segsel[segnum])].limit;
    unsigned n = u16(rel);
    for (unsigned r = 0; r < n; r++) {
        const uint8_t *rec = rel + 2 + 8 * r;
        unsigned atype = rec[0], rtype = rec[1] & 3, additive = rec[1] & 4;
        uint16_t loc = u16(rec + 2), t1 = u16(rec + 4), t2 = u16(rec + 6);
        uint16_t sel = 0, off = 0;
        switch (rtype) {
        case 0:                                    /* internal reference */
            if ((t1 & 0xFF) == 0xFF) {
                if (!module_entry(m, t2, &sel, &off))
                    rt_fatal("%s: bad movable entry %u", m->name, t2);
            } else {
                sel = m->segsel[t1 & 0xFF];
                off = t2;
            }
            break;
        case 1:                                    /* import by ordinal */
            resolve_import(m, t1, t2, NULL, &sel, &off);
            break;
        case 2: {                                  /* import by name */
            char name[256];
            pstr(ne + u16(ne + 0x2A) + t2, name, sizeof(name));
            resolve_import(m, t1, 0, name, &sel, &off);
            break;
        }
        case 3:                                    /* OS fixup: FPU emulation, ignored (real FPU) */
            continue;
        }
        for (;;) {
            if (loc > limit)
                rt_fatal("%s: relocation at %u:%04x outside segment", m->name, segnum, loc);
            uint8_t *p = mem + loc;
            uint16_t next = u16(p);
            switch (atype) {
            case 0:                                /* low byte */
                p[0] = (uint8_t)(additive ? p[0] + off : off);
                break;
            case 2:                                /* selector */
                p[0] = (uint8_t)sel; p[1] = (uint8_t)(sel >> 8);
                break;
            case 3: {                              /* far pointer */
                uint16_t o = additive ? (uint16_t)(u16(p) + off) : off;
                p[0] = (uint8_t)o; p[1] = (uint8_t)(o >> 8);
                p[2] = (uint8_t)sel; p[3] = (uint8_t)(sel >> 8);
                break;
            }
            case 5: {                              /* offset */
                uint16_t o = additive ? (uint16_t)(u16(p) + off) : off;
                p[0] = (uint8_t)o; p[1] = (uint8_t)(o >> 8);
                break;
            }
            case 11: case 13:                      /* 48-bit pointer / 32-bit offset: unused here */
            default:
                rt_fatal("%s: unsupported relocation address type %u", m->name, atype);
            }
            if (additive || atype == 0 || next == 0xFFFF)
                break;
            loc = next;
        }
    }
}

LoadedModule *ne_load(const char *path, const RecompModule *rm)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOGE("can't open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    LoadedModule *m = calloc(1, sizeof(*m));
    m->file = malloc((size_t)size);
    m->file_size = (size_t)size;
    if (fread(m->file, 1, (size_t)size, f) != (size_t)size)
        rt_fatal("short read on %s", path);
    fclose(f);
    /* the shipped PLAY_E.EXE is packed with SLR's OPTLOADER: unpack it as its loader would */
    if (size >= 0x40 && m->file[0] == 'M' && m->file[1] == 'Z') {
        uint32_t neo = u32(m->file + 0x3C);
        if (neo + 0x10 < (uint32_t)size && m->file[neo] == 'N' && m->file[neo + 1] == 'E' &&
            (u16(m->file + neo + 0x0C) & 0x0800)) {
            uint8_t *img;
            size_t img_n;
            char err[160];
            if (!optloader_unpack(m->file, (size_t)size, &img, &img_n, err, sizeof(err)))
                rt_fatal("%s could not be unpacked (%s).", path, err);
            free(m->file);
            m->file = img;
            m->file_size = img_n;
            size = (long)img_n;
            const char *dump = getenv("COK_DUMP_UNPACKED");      /* debugging: compare with tools/unpack.py */
            FILE *df = dump ? fopen(dump, "wb") : NULL;
            if (df) {
                fwrite(img, 1, img_n, df);
                fclose(df);
            }
        }
    }
    /* the generated code only fits the exact file it was made from */
    if (rm && rm->image_size) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < m->file_size; i++)
            h = (h ^ m->file[i]) * 0x100000001B3ull;
        if (m->file_size != rm->image_size || h != rm->image_hash)
            rt_fatal("%s is not the version this build was made from.\n\n"
                     "cok.exe runs the English Windows release of Cosmology of Kyoto: its PLAY_E.EXE, "
                     "FILEIO.DLL and CK_DATA folder, as on the CD.", path);
    }
    snprintf(m->path, sizeof(m->path), "%s", path);
    const char *base = path + strlen(path);
    while (base > path && base[-1] != '/' && base[-1] != '\\')
        base--;
    snprintf(m->guest_path, sizeof(m->guest_path), "%s\\%s", GUEST_DIR, base);
    m->rm = rm;

    if (size < 0x40 || m->file[0] != 'M' || m->file[1] != 'Z')
        rt_fatal("%s: not an MZ executable", path);
    m->ne = (uint16_t)u32(m->file + 0x3C);
    const uint8_t *ne = m->file + m->ne;
    if (ne[0] != 'N' || ne[1] != 'E')
        rt_fatal("%s: not an NE executable", path);
    m->flags = u16(ne + 0x0C);
    if (m->flags & 0x0800)
        rt_fatal("%s is self-loading; load the unpacked file from tools/unpack.py", path);
    m->autodata = u16(ne + 0x0E);
    m->heap = u16(ne + 0x10);
    m->stack = u16(ne + 0x12);
    m->ip = u16(ne + 0x14);
    m->cs = u16(ne + 0x16);
    m->sp = u16(ne + 0x18);
    m->ss = u16(ne + 0x1A);
    m->nseg = u16(ne + 0x1C);
    pstr(ne + u16(ne + 0x26), m->name, sizeof(m->name));
    unsigned align = u16(ne + 0x32) ? u16(ne + 0x32) : 9;
    if (rm && rm->nseg != m->nseg)
        rt_fatal("%s has %u segments but the recompiled module expects %u", path, m->nseg, rm->nseg);

    m->segs = calloc(m->nseg + 1u, sizeof(NESeg));
    m->segsel = calloc(m->nseg + 1u, sizeof(uint16_t));
    const uint8_t *st = ne + u16(ne + 0x22);
    for (unsigned i = 1; i <= m->nseg; i++, st += 8) {
        NESeg *s = &m->segs[i];
        s->sector = u16(st);
        s->length = u16(st + 2);
        s->flags = u16(st + 4);
        s->minalloc = u16(st + 6);
        uint32_t alloc = s->minalloc ? s->minalloc : 0x10000;
        if (i == m->autodata) {
            alloc += m->heap + m->stack;           /* local heap and stack live in DGROUP */
            if (alloc > 0x10000)
                alloc = 0x10000;
        }
        uint16_t sel = sel_alloc(alloc, (s->flags & 1) ? SEL_DATA : SEL_CODE);
        if (!sel)
            rt_fatal("%s: can't allocate segment %u", path, i);
        g_sel[SEL_INDEX(sel)].module = m;
        g_sel[SEL_INDEX(sel)].segnum = (uint16_t)i;
        m->segsel[i] = sel;
        if (s->sector) {
            uint32_t len = s->length ? s->length : 0x10000;
            size_t start = (size_t)s->sector << align;
            if (start + len > m->file_size)
                rt_fatal("%s: segment %u extends past end of file", path, i);
            memcpy(g_seg_base[SEL_INDEX(sel)], m->file + start, len);
        }
    }
    parse_entries(m);
    m->hmodule = m->autodata ? m->segsel[m->autodata] : m->segsel[1];

    if (g_nmodules == MAX_MODULES)
        rt_fatal("too many modules");
    g_modules[g_nmodules++] = m;                    /* registered before relocating: self-imports */

    st = ne + u16(ne + 0x22);
    for (unsigned i = 1; i <= m->nseg; i++, st += 8) {
        NESeg *s = &m->segs[i];
        if (s->sector && (s->flags & 0x0100)) {
            uint32_t len = s->length ? s->length : 0x10000;
            apply_relocs(m, i, m->file + ((size_t)s->sector << align) + len);
        }
    }

    if (rm) {
        for (unsigned i = 1; i <= m->nseg; i++)
            rm->sel[i] = m->segsel[i];
        for (uint32_t i = 0; i < rm->nimports; i++) {
            const RecompImport *imp = &rm->imports[i];
            LoadedModule *target = module_by_name(imp->module);
            uint16_t sel = 0, off = 0, ord = imp->ordinal, value;
            if (target) {
                if (imp->name)
                    module_export_by_name(target, imp->name, &ord);
                module_entry(target, ord, &sel, &off);
                rm->import_slot[i] = 0xFFFF;       /* call goes through rt_call_far */
            } else if (!imp->name && api_constant(imp->module, ord, &value)) {
                off = value;
                rm->import_slot[i] = 0xFFFF;
            } else {
                int slot = api_slot(imp->module, ord, imp->name, imp->label);
                rm->import_slot[i] = (uint16_t)slot;
                uint32_t addr = api_slot_addr(slot);
                sel = (uint16_t)(addr >> 16);
                off = (uint16_t)addr;
            }
            rm->import_addr[i] = (uint32_t)sel << 16 | off;
        }
    }
    LOGI("loaded %s (%s): %u segments, DGROUP %04x", m->name, path, m->nseg,
         m->autodata ? m->segsel[m->autodata] : 0);
    return m;
}
