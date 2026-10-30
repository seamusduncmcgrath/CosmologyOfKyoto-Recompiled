/* DOS services (int 21h and KERNEL DOS3Call) and the guest file system mapping.
 *
 * The program believes it lives in C:\COK (GUEST_DIR); that and the CD drive D: map to the
 * host directory holding the CD contents. Unimplemented functions stop with a message
 * naming them, like unimplemented APIs.
 */
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"

char g_game_dir[512] = ".";
char g_win_dir[512];                             /* host directory for C:\WINDOWS, if any */
char g_save_dir[512];                            /* host directory for C:\SAVES, if any */
static char g_mount[26][512];                    /* drives E: to Z: mounted by file dialogs */
static char g_cwd[128] = GUEST_DIR;              /* guest current directory */

void vfs_full_path(const char *guest, char *out, size_t cap)
{
    char full[260];
    if (guest[0] && guest[1] == ':' && (guest[2] == '\\' || guest[2] == '/'))
        snprintf(full, sizeof(full), "%s", guest);
    else if (guest[0] && guest[1] == ':')        /* drive-relative: only C: has a cwd */
        snprintf(full, sizeof(full), "%c:\\%s", guest[0], guest + 2);
    else if (guest[0] == '\\' || guest[0] == '/')
        snprintf(full, sizeof(full), "%c:%s", g_cwd[0], guest);
    else
        snprintf(full, sizeof(full), "%s\\%s", g_cwd, guest);
    /* upper case, '/' to backslash, and resolve "." and ".." components */
    size_t o = 0;
    for (size_t i = 0; full[i] && o < cap - 1;) {
        char ch = full[i] == '/' ? '\\' : (char)toupper((unsigned char)full[i]);
        if (ch == '\\' && o > 0) {
            const char *comp = full + i + 1;
            size_t len = strcspn(comp, "\\/");
            if (len == 0 || (len == 1 && comp[0] == '.')) {
                i += 1 + len;
                continue;
            }
            if (len == 2 && comp[0] == '.' && comp[1] == '.') {
                while (o > 2 && out[o - 1] != '\\')
                    o--;
                if (o > 2)
                    o--;
                i += 1 + len;
                continue;
            }
        }
        out[o++] = ch;
        i++;
    }
    if (o == 2)                                  /* "C:" alone means the root */
        out[o++] = '\\';
    out[o] = 0;
}

static int under(const char *full, const char *dir, const char **rest)
{
    size_t n = strlen(dir);
    if (strncmp(full, dir, n) != 0 || (full[n] != '\\' && full[n] != 0))
        return 0;
    *rest = full + n;
    return 1;
}

/* DOS names are case-insensitive and the program upper-cases them; the host's may not be
 * (Linux, a case-sensitive macOS volume, a copy of the CD with lower-case names). When the
 * path doesn't exist as spelled, match each component after `fixed` against the directory's
 * entries ignoring case. A component with no match (a file about to be created) and anything
 * after it stay as they are. */
static void resolve_case(char *path, size_t cap, size_t fixed)
{
    if (SDL_GetPathInfo(path, NULL))
        return;
    size_t start = fixed;
    while (path[start] == '/')
        start++;
    while (path[start]) {
        size_t end = start;
        while (path[end] && path[end] != '/')
            end++;
        char save = path[end];
        path[end] = 0;
        int found = SDL_GetPathInfo(path, NULL);
        if (!found) {
            char *slash = strrchr(path, '/');
            const char *dir = path, *name = path + start;
            char parent[1100];
            if (slash && slash > path) {
                snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
                dir = parent;
            } else {
                dir = slash ? "/" : ".";
            }
            int n = 0;
            char **match = strpbrk(name, "*?[") ? NULL : SDL_GlobDirectory(dir, name, SDL_GLOB_CASEINSENSITIVE, &n);
            if (match && n > 0 && strlen(match[0]) == end - start) {
                memcpy(path + start, match[0], end - start);   /* same length: only the case differs */
                found = 1;
            }
            SDL_free(match);
        }
        path[end] = save;
        if (!found || !save || end + 1 >= cap)
            return;
        start = end + 1;
    }
}

int vfs_host_path(const char *guest, char *out, size_t cap)
{
    char full[260];
    vfs_full_path(guest, full, sizeof(full));
    const char *rest, *base;
    if (under(full, GUEST_DIR, &rest))
        base = g_game_dir;
    else if (full[0] == GUEST_CD_DRIVE && full[1] == ':')
        base = g_game_dir, rest = full + 2;
    else if (g_win_dir[0] && under(full, "C:\\WINDOWS", &rest))
        base = g_win_dir;
    else if (g_save_dir[0] && under(full, "C:\\SAVES", &rest))
        base = g_save_dir;
    else if (full[0] >= 'E' && full[0] <= 'Z' && full[1] == ':' && g_mount[full[0] - 'A'][0])
        base = g_mount[full[0] - 'A'], rest = full + 2;
    else
        return 0;
    while (*rest == '\\')
        rest++;
    snprintf(out, cap, "%s/%s", base, rest);
    for (char *p = out + strlen(base); *p; p++)
        if (*p == '\\')
            *p = '/';
    resolve_case(out, cap, strlen(base));
    return 1;
}

/* An absolute host path with '/' separators, no "." components and no trailing separator. */
static void host_abs(const char *in, char *out, size_t cap)
{
    char tmp[1100];
    int absolute = in[0] == '/' || in[0] == '\\' || (in[0] && in[1] == ':');
    if (absolute) {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else {
        char *cwd = SDL_GetCurrentDirectory();
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd ? cwd : "", in);
        SDL_free(cwd);
    }
    size_t o = 0;
    for (size_t i = 0; tmp[i] && o < cap - 1;) {
        char ch = tmp[i] == '\\' ? '/' : tmp[i];
        if (ch == '/') {
            const char *comp = tmp + i + 1;
            if (comp[0] == '.' && (comp[1] == '/' || comp[1] == '\\' || !comp[1])) {
                i += 2;
                continue;
            }
            if (o && out[o - 1] == '/') {
                i++;
                continue;
            }
        }
        out[o++] = ch;
        i++;
    }
    while (o > 1 && out[o - 1] == '/' && !(o == 3 && out[1] == ':'))
        o--;
    out[o] = 0;
}

int vfs_drive_mounted(int letter)
{
    letter = toupper(letter);
    return letter >= 'E' && letter <= 'Z' && g_mount[letter - 'A'][0];
}

/* The guest path for a host file (one picked in a file dialog). A file outside the folders
 * the guest sees gets its folder mounted as a drive letter, which also keeps paths short. */
int vfs_guest_path(const char *host, char *out, size_t cap)
{
    static char letters[26][3];
    char file[1100], dir[1100];
    host_abs(host, file, sizeof(file));
    struct { const char *guest, *host; } roots[3 + 26] = {
        { GUEST_DIR, g_game_dir }, { "C:\\WINDOWS", g_win_dir }, { "C:\\SAVES", g_save_dir } };
    int n = 3;
    for (int d = 'E'; d <= 'Z'; d++)
        if (g_mount[d - 'A'][0]) {
            snprintf(letters[d - 'A'], 3, "%c:", d);
            roots[n].guest = letters[d - 'A'];
            roots[n++].host = g_mount[d - 'A'];
        }
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < n; i++) {
        if (!roots[i].host[0])
            continue;
        host_abs(roots[i].host, dir, sizeof(dir));
        size_t len = strlen(dir);
        if (SDL_strncasecmp(file, dir, len) == 0 && file[len] == '/' && len > best_len)
            best = i, best_len = len;
    }
    if (best < 0) {                                /* mount the file's folder */
        char *slash = strrchr(file, '/');
        int d = 'E';
        while (d <= 'Z' && g_mount[d - 'A'][0])
            d++;
        if (!slash || d > 'Z')
            return 0;
        snprintf(g_mount[d - 'A'], sizeof(g_mount[0]), "%.*s", (int)(slash - file), file);
        LOGI("mounted %s as %c:", g_mount[d - 'A'], d);
        snprintf(letters[d - 'A'], 3, "%c:", d);
        roots[n].guest = letters[d - 'A'];
        best = n++;
        best_len = (size_t)(slash - file);
    }
    snprintf(out, cap, "%s%s", roots[best].guest, file + best_len);
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    return 1;
}

static void dos_error(CPU *c, uint16_t code)
{
    c->a.x = code;
    c->cf = 1;
}

/* ---- file handles ------------------------------------------------------------------------ */

#define DOS_ERR_NOFILE 2
#define DOS_ERR_NOPATH 3
#define DOS_ERR_NOHANDLES 4
#define DOS_ERR_ACCESS 5
#define DOS_ERR_HANDLE 6
#define MAX_HANDLES 64
#define FIRST_FILE_HANDLE 5        /* 0-4: stdin, stdout, stderr, aux, prn */

static FILE *g_files[MAX_HANDLES];
static char g_file_names[MAX_HANDLES][64];   /* guest names, for logs */

static FILE *handle_file(uint16_t h)
{
    return h < MAX_HANDLES ? g_files[h] : NULL;
}

/* Open a guest path; mode: 0 read, 1 write, 2 read/write, 3 create (truncate). Returns a DOS
 * handle or a negative DOS error code. Shared with KERNEL _lopen/_lcreat/OpenFile. */
int dos_open(const char *guest, int mode)
{
    char host[600];
    if (!vfs_host_path(guest, host, sizeof(host))) {
        LOGD("open %s: path not mapped", guest);
        return -DOS_ERR_NOPATH;
    }
    static const char *const fmodes[] = { "rb", "r+b", "r+b", "w+b" };
    FILE *f = fopen(host, fmodes[mode & 3]);
    if (!f && (mode & 3) == 1)
        f = fopen(host, "wb");
    if (!f) {
        LOGD("open %s (%s) mode %d: not found", guest, host, mode);
        return -DOS_ERR_NOFILE;
    }
    for (int h = FIRST_FILE_HANDLE; h < MAX_HANDLES; h++) {
        if (!g_files[h]) {
            g_files[h] = f;
            snprintf(g_file_names[h], sizeof(g_file_names[h]), "%s", guest);
            LOGD("open %s mode %d = handle %d", guest, mode, h);
            return h;
        }
    }
    fclose(f);
    return -DOS_ERR_NOHANDLES;
}

int dos_close(uint16_t h)
{
    FILE *f = handle_file(h);
    if (!f)
        return h < FIRST_FILE_HANDLE ? 0 : -DOS_ERR_HANDLE;
    fclose(f);
    g_files[h] = NULL;
    return 0;
}

long dos_read(uint16_t h, void *buf, uint16_t n)
{
    FILE *f = handle_file(h);
    if (!f)
        return h == 0 ? 0 : -DOS_ERR_HANDLE;
    return (long)fread(buf, 1, n, f);
}

long dos_write(uint16_t h, const void *buf, uint16_t n)
{
    if (h == 1 || h == 2) {                         /* stdout/stderr: to our log */
        fwrite(buf, 1, n, stderr);
        return n;
    }
    FILE *f = handle_file(h);
    if (!f)
        return h < FIRST_FILE_HANDLE ? n : -DOS_ERR_HANDLE;
    return (long)fwrite(buf, 1, n, f);
}

long dos_seek(uint16_t h, int32_t off, int origin)
{
    FILE *f = handle_file(h);
    if (!f)
        return -DOS_ERR_HANDLE;
    if (fseek(f, off, origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET) != 0)
        return -25;                                 /* seek error */
    return ftell(f);
}

/* ---- find first / find next (with the DTA) ------------------------------------------------ */

static uint32_t g_dta;                             /* far pointer set by AH=1Ah */

typedef struct FindEntry {
    char name[13];                                 /* 8.3, upper case */
    uint8_t attr;
    uint32_t size;
    uint16_t time, date;
} FindEntry;

typedef struct FindState {
    FindEntry *e;
    int n, cap, next;
} FindState;

#define MAX_FINDS 8
static FindState g_find[MAX_FINDS];
static int g_find_rr;

/* DOS wildcard match of an 8.3 name against an 8.3 pattern, part by part */
static int match_part(const char *name, size_t nl, const char *pat, size_t pl)
{
    size_t i = 0, j = 0;
    for (; j < pl; j++) {
        if (pat[j] == '*')
            return 1;
        if (pat[j] == '?') {
            if (i < nl) i++;
            continue;
        }
        if (i >= nl || toupper((unsigned char)name[i]) != toupper((unsigned char)pat[j]))
            return 0;
        i++;
    }
    return i == nl;
}

static int match83(const char *name, const char *pat)
{
    const char *nd = strchr(name, '.'), *pd = strchr(pat, '.');
    size_t nb = nd ? (size_t)(nd - name) : strlen(name), pb = pd ? (size_t)(pd - pat) : strlen(pat);
    const char *ne = nd ? nd + 1 : "", *pe = pd ? pd + 1 : "";
    return match_part(name, nb, pat, pb) && match_part(ne, strlen(ne), pe, strlen(pe));
}

static int is83(const char *s)
{
    const char *dot = strchr(s, '.');
    size_t base = dot ? (size_t)(dot - s) : strlen(s), ext = dot ? strlen(dot + 1) : 0;
    if (!base || base > 8 || ext > 3 || (dot && strchr(dot + 1, '.')))
        return 0;
    for (const char *p = s; *p; p++)
        if (*p == ' ' || (unsigned char)*p > 0x7E)
            return 0;
    return 1;
}

typedef struct FindCtx {
    FindState *st;
    const char *dir;
    const char *pat;
    uint16_t attr;
} FindCtx;

static SDL_EnumerationResult SDLCALL find_cb(void *ud, const char *dirname, const char *fname)
{
    FindCtx *fc = ud;
    if (!strcmp(fname, ".") || !strcmp(fname, "..") || !is83(fname) || !match83(fname, fc->pat))
        return SDL_ENUM_CONTINUE;
    char full[700];
    snprintf(full, sizeof(full), "%s/%s", fc->dir, fname);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(full, &info))
        return SDL_ENUM_CONTINUE;
    uint8_t attr = info.type == SDL_PATHTYPE_DIRECTORY ? 0x10 : 0x20;
    if ((attr & 0x10) && !(fc->attr & 0x10))
        return SDL_ENUM_CONTINUE;
    FindState *st = fc->st;
    if (st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 32;
        st->e = realloc(st->e, sizeof(FindEntry) * (size_t)st->cap);
    }
    FindEntry *e = &st->e[st->n++];
    memset(e, 0, sizeof(*e));
    for (int i = 0; fname[i] && i < 12; i++)
        e->name[i] = (char)toupper((unsigned char)fname[i]);
    e->attr = attr;
    e->size = attr & 0x10 ? 0 : (uint32_t)info.size;
    SDL_DateTime dt;
    if (SDL_TimeToDateTime(info.modify_time, &dt, true)) {
        e->time = (uint16_t)(dt.hour << 11 | dt.minute << 5 | dt.second / 2);
        e->date = (uint16_t)((dt.year - 1980) << 9 | dt.month << 5 | dt.day);
    }
    (void)dirname;
    return SDL_ENUM_CONTINUE;
}

static int find_fill(FindState *st, int slot)
{
    if (st->next >= st->n)
        return 0;
    FindEntry *e = &st->e[st->next++];
    uint16_t s = FAR_SEL(g_dta), o = FAR_OFF(g_dta);
    wr8(s, o, (uint8_t)slot);
    wr8(s, o + 0x15, e->attr);
    wr16(s, o + 0x16, e->time);
    wr16(s, o + 0x18, e->date);
    wr32(s, o + 0x1A, e->size);
    memset(mem_ptr(s, o + 0x1E), 0, 13);
    memcpy(mem_ptr(s, o + 0x1E), e->name, strlen(e->name));
    return 1;
}

static void find_first(CPU *c, const char *spec, uint16_t attr)
{
    char full[260];
    vfs_full_path(spec, full, sizeof(full));
    char *slash = strrchr(full, '\\');
    const char *pat = slash ? slash + 1 : full;
    char dir_guest[260];
    snprintf(dir_guest, sizeof(dir_guest), "%.*s", slash ? (int)(slash - full) : 0, full);
    if (strlen(dir_guest) == 2)                    /* "C:" */
        strcat(dir_guest, "\\");
    char host[600];
    int slot = g_find_rr++ % MAX_FINDS;
    FindState *st = &g_find[slot];
    st->n = st->next = 0;
    if (!strcmp(dir_guest, "C:\\")) {              /* the root of C: holds only our directories */
        static const char *const dirs[] = { "COK", "WINDOWS", "SAVES" };
        for (int i = 0; i < 3; i++) {
            if (!(attr & 0x10) || !match83(dirs[i], pat) || (i == 1 && !g_win_dir[0]) || (i == 2 && !g_save_dir[0]))
                continue;
            if (st->n == st->cap) {
                st->cap = st->cap ? st->cap * 2 : 32;
                st->e = realloc(st->e, sizeof(FindEntry) * (size_t)st->cap);
            }
            FindEntry *e = &st->e[st->n++];
            memset(e, 0, sizeof(*e));
            strcpy(e->name, dirs[i]);
            e->attr = 0x10;
            e->date = (uint16_t)((1995 - 1980) << 9 | 1 << 5 | 1);
        }
        LOGD("find first %s: %d matches", spec, st->n);
        if (!find_fill(st, slot))
            dos_error(c, 18);
        return;
    }
    if (!vfs_host_path(dir_guest, host, sizeof(host))) {
        dos_error(c, DOS_ERR_NOPATH);
        return;
    }
    FindCtx fc = { st, host, pat, attr };
    SDL_EnumerateDirectory(host, find_cb, &fc);
    LOGD("find first %s: %d matches", spec, st->n);
    if (!find_fill(st, slot))
        dos_error(c, 18);                          /* no more files */
}

static void find_next(CPU *c)
{
    int slot = rd8(FAR_SEL(g_dta), FAR_OFF(g_dta));
    if (slot < 0 || slot >= MAX_FINDS || !find_fill(&g_find[slot], slot))
        dos_error(c, 18);
}

void dos_int21(CPU *c)
{
    uint8_t ah = c->a.b.h;
    c->cf = 0;
    if (ah == 0x3B || ah == 0x3C || ah == 0x3D || ah == 0x41 || ah == 0x43 || ah == 0x4E || ah == 0x56)
        LOGD("int 21h AH=%02xh %s", ah, (const char *)mem_ptr(c->ds, c->d.x));
    else
        LOGD("int 21h AX=%04xh BX=%04x", c->a.x, c->b.x);
    switch (ah) {
    case 0x36:                                       /* disk free: report ~128 MB free */
        c->a.x = 8;                                  /* sectors per cluster */
        c->b.x = 0x8000;                             /* free clusters */
        c->c.x = 512;                                /* bytes per sector */
        c->d.x = 0xFFFF;                             /* total clusters */
        break;
    case 0x3B: {                                     /* chdir DS:DX */
        const char *p = (const char *)mem_ptr(c->ds, c->d.x);
        char host[600];
        if (!vfs_host_path(p, host, sizeof(host))) {
            dos_error(c, DOS_ERR_NOPATH);
            break;
        }
        vfs_full_path(p, g_cwd, sizeof(g_cwd));
        break;
    }
    case 0x3C: case 0x3D: {                          /* create / open DS:DX */
        const char *p = (const char *)mem_ptr(c->ds, c->d.x);
        int h = dos_open(p, ah == 0x3C ? 3 : c->a.b.l & 3);
        if (h < 0)
            dos_error(c, (uint16_t)-h);
        else
            c->a.x = (uint16_t)h;
        break;
    }
    case 0x3E: {                                     /* close BX */
        int r = dos_close(c->b.x);
        if (r < 0)
            dos_error(c, (uint16_t)-r);
        break;
    }
    case 0x3F: case 0x40: {                          /* read / write BX, CX bytes at DS:DX */
        void *buf = mem_ptr(c->ds, c->d.x);
        long r = ah == 0x3F ? dos_read(c->b.x, buf, c->c.x) : dos_write(c->b.x, buf, c->c.x);
        if (r < 0)
            dos_error(c, (uint16_t)-r);
        else
            c->a.x = (uint16_t)r;
        break;
    }
    case 0x41: {                                     /* delete DS:DX */
        char host[600];
        if (!vfs_host_path((const char *)mem_ptr(c->ds, c->d.x), host, sizeof(host)) || remove(host) != 0)
            dos_error(c, DOS_ERR_NOFILE);
        break;
    }
    case 0x42: {                                     /* seek BX to CX:DX from AL */
        long r = dos_seek(c->b.x, (int32_t)((uint32_t)c->c.x << 16 | c->d.x), c->a.b.l);
        if (r < 0) {
            dos_error(c, (uint16_t)-r);
        } else {
            c->a.x = (uint16_t)r;
            c->d.x = (uint16_t)(r >> 16);
        }
        break;
    }
    case 0x43: {                                     /* get/set attributes of DS:DX */
        char host[600];
        FILE *f = NULL;
        if (vfs_host_path((const char *)mem_ptr(c->ds, c->d.x), host, sizeof(host)))
            f = fopen(host, "rb");
        if (!f) {
            dos_error(c, DOS_ERR_NOFILE);
            break;
        }
        fclose(f);
        if (c->a.b.l == 0)
            c->c.x = 0x20;                           /* archive */
        break;
    }
    case 0x47: {                                     /* get cwd of drive DL into DS:SI */
        char *out = (char *)mem_ptr(c->ds, c->si.x);
        const char *p = g_cwd + 2;                   /* skip "C:" */
        while (*p == '\\')
            p++;
        snprintf(out, 64, "%s", p);
        c->a.x = 0x0100;
        break;
    }
    case 0x56: {                                     /* rename DS:DX -> ES:DI */
        char from[600], to[600];
        if (!vfs_host_path((const char *)mem_ptr(c->ds, c->d.x), from, sizeof(from)) ||
            !vfs_host_path((const char *)mem_ptr(c->es, c->di.x), to, sizeof(to)) || rename(from, to) != 0)
            dos_error(c, DOS_ERR_ACCESS);
        break;
    }
    case 0x68: {                                     /* commit BX */
        FILE *f = handle_file(c->b.x);
        if (f)
            fflush(f);
        break;
    }
    case 0x1A:                                       /* set DTA = DS:DX */
        g_dta = (uint32_t)c->ds << 16 | c->d.x;
        break;
    case 0x2F:                                       /* get DTA -> ES:BX */
        c->es = FAR_SEL(g_dta);
        c->b.x = FAR_OFF(g_dta);
        break;
    case 0x4E:                                       /* find first DS:DX, attributes CX */
        find_first(c, (const char *)mem_ptr(c->ds, c->d.x), c->c.x);
        break;
    case 0x4F:                                       /* find next */
        find_next(c);
        break;
    case 0x0D:                                       /* disk reset */
        break;
    case 0x0E:                                       /* select drive: AL = number of drives */
        c->a.b.l = 5;
        break;
    case 0x19:                                       /* get current drive: C: */
        c->a.b.l = (uint8_t)(toupper((unsigned char)g_cwd[0]) - 'A');
        break;
    case 0x25:                                       /* set interrupt vector: ignored */
        break;
    case 0x2A: {                                     /* get date */
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        c->c.x = (uint16_t)(tm->tm_year + 1900);
        c->d.b.h = (uint8_t)(tm->tm_mon + 1);
        c->d.b.l = (uint8_t)tm->tm_mday;
        c->a.b.l = (uint8_t)tm->tm_wday;
        break;
    }
    case 0x2C: {                                     /* get time */
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        c->c.b.h = (uint8_t)tm->tm_hour;
        c->c.b.l = (uint8_t)tm->tm_min;
        c->d.b.h = (uint8_t)tm->tm_sec;
        c->d.b.l = 0;
        break;
    }
    case 0x30:                                       /* DOS version 5.00 */
        c->a.x = 0x0005;
        c->b.x = 0;
        c->c.x = 0;
        break;
    case 0x35:                                       /* get interrupt vector: none */
        c->es = 0;
        c->b.x = 0;
        break;
    case 0x44:                                       /* IOCTL */
        if (c->a.b.l == 0) {                         /* get device info */
            c->d.x = c->b.x <= 2 ? 0x80D3 : 0x0002;  /* std handles are character devices */
            break;
        }
        dos_error(c, 1);
        break;
    case 0x4C:                                       /* exit */
        LOGI("program exited with code %u", c->a.b.l);
        exit(c->a.b.l);
    default:
        rt_fatal("unimplemented DOS function int 21h AH=%02xh (AX=%04x)", ah, c->a.x);
    }
}
