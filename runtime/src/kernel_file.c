/* KERNEL file functions (_lopen and friends, OpenFile), directories and profile (.INI)
 * files.
 *
 * Profile files: a name without a path lives in the Windows directory, as in Windows 3.1.
 * WIN.INI is built in (the settings a 1995 machine would have had); other files are read
 * from the guest file system and written back when changed.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define HFILE_ERROR 0xFFFF

/* ---- _lopen / _lread / ... ------------------------------------------------------------------ */

static void lopen(CPU *c)              /* HFILE _lopen(LPCSTR, int mode) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    int h = dos_open(guest_ptr(a[0]), (int)(a[1] & 3));
    LOGD("_lopen(%s, %u) = %d", guest_ptr(a[0]), a[1], h);
    c->a.x = h < 0 ? HFILE_ERROR : (uint16_t)h;
    api_ret(c, n);
}

static void lcreat(CPU *c)             /* HFILE _lcreat(LPCSTR, int attr) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    int h = dos_open(guest_ptr(a[0]), 3);
    LOGD("_lcreat(%s) = %d", guest_ptr(a[0]), h);
    c->a.x = h < 0 ? HFILE_ERROR : (uint16_t)h;
    api_ret(c, n);
}

static void lclose(CPU *c)             /* HFILE _lclose(HFILE): 0 or HFILE_ERROR */
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    c->a.x = dos_close((uint16_t)a[0]) < 0 ? HFILE_ERROR : 0;
    api_ret(c, n);
}

static void lread(CPU *c)              /* UINT _lread(HFILE, void huge *, UINT) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    long r = dos_read((uint16_t)a[0], guest_ptr(a[1]), (uint16_t)a[2]);
    LOGD("_lread(%u, %04x:%04x, %u) = %ld", a[0], FAR_SEL(a[1]), FAR_OFF(a[1]), a[2], r);
    c->a.x = r < 0 ? HFILE_ERROR : (uint16_t)r;
    api_ret(c, n);
}

static void lwrite(CPU *c)             /* UINT _lwrite(HFILE, const void huge *, UINT) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    long r = dos_write((uint16_t)a[0], guest_ptr(a[1]), (uint16_t)a[2]);
    c->a.x = r < 0 ? HFILE_ERROR : (uint16_t)r;
    api_ret(c, n);
}

static void llseek(CPU *c)             /* LONG _llseek(HFILE, LONG, int origin) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "wlw", a);
    long r = dos_seek((uint16_t)a[0], (int32_t)a[1], (int)a[2]);
    LOGD("_llseek(%u, %d, %u) = %ld", a[0], (int32_t)a[1], a[2], r);
    api_ret32(c, n, r < 0 ? 0xFFFFFFFFu : (uint32_t)r);
}

/* OpenFile style bits */
#define OF_PARSE 0x0100
#define OF_DELETE 0x0200
#define OF_CREATE 0x1000
#define OF_EXIST 0x4000

/* HFILE OpenFile(LPCSTR, OFSTRUCT FAR *, UINT style). OFSTRUCT: cBytes, fFixedDisk,
 * nErrCode (word), 4 reserved bytes, szPathName[128]. */
static void OpenFile(CPU *c)
{
    uint32_t a[3];
    uint16_t n = api_args(c, "llw", a);
    const char *name = guest_ptr(a[0]);
    uint8_t *of = (uint8_t *)guest_ptr(a[1]);
    uint16_t style = (uint16_t)a[2];
    char full[128];
    vfs_full_path(name, full, sizeof(full));
    memset(of, 0, 136);
    of[0] = 136;
    of[1] = 1;
    snprintf((char *)of + 8, 128, "%s", full);
    int h = 0;
    uint16_t err = 0;
    if (style & OF_PARSE) {
        h = 0;
    } else if (style & OF_DELETE) {
        char host[600];
        if (!vfs_host_path(full, host, sizeof(host)) || remove(host) != 0)
            err = 2;
        h = err ? -1 : 1;
    } else {
        h = dos_open(full, (style & OF_CREATE) ? 3 : (int)(style & 3));
        if (h < 0) {
            err = (uint16_t)-h;
        } else if (style & OF_EXIST) {
            dos_close((uint16_t)h);
            h = 1;                                 /* Windows returns a (closed) handle; nonzero */
        }
    }
    of[2] = (uint8_t)err;
    of[3] = (uint8_t)(err >> 8);
    LOGD("OpenFile(%s, %04x) = %d (%s)", name, style, h, full);
    c->a.x = err ? HFILE_ERROR : (uint16_t)h;
    api_ret(c, n);
}

static void GetWindowsDirectory(CPU *c)    /* UINT GetWindowsDirectory(LPSTR, UINT) */
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    const char *dir = "C:\\WINDOWS";
    size_t len = strlen(dir);
    if (a[1] > len)
        memcpy(guest_ptr(a[0]), dir, len + 1);
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

static void GetSystemDirectory(CPU *c)
{
    uint32_t a[2];
    uint16_t n = api_args(c, "lw", a);
    const char *dir = "C:\\WINDOWS\\SYSTEM";
    size_t len = strlen(dir);
    if (a[1] > len)
        memcpy(guest_ptr(a[0]), dir, len + 1);
    c->a.x = (uint16_t)len;
    api_ret(c, n);
}

/* UINT GetDriveType(int drive), 0 = A:. A CD-ROM drive reports DRIVE_REMOTE under Win16. */
static void GetDriveType(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "w", a);
    static const uint16_t types[] = { 2, 0, 3, GUEST_CD_DRIVE_TYPE };
    c->a.x = a[0] < 4 ? types[a[0]] : vfs_drive_mounted('A' + (int)a[0]) ? 3 : 0;
    api_ret(c, n);
}

/* ---- profile files --------------------------------------------------------------------------- */

static const char g_win_ini_default[] =
    "[windows]\r\n"
    "load=\r\nrun=\r\nBeep=yes\r\nSpooler=yes\r\nNullPort=None\r\n"
    "device=\r\n"
    "BorderWidth=3\r\nKeyboardSpeed=31\r\nCursorBlinkRate=530\r\nDoubleClickSpeed=452\r\n"
    "Programs=com exe bat pif\r\nDocuments=\r\n"
    "[Desktop]\r\nPattern=(None)\r\nWallpaper=(None)\r\nGridGranularity=0\r\n"
    "[intl]\r\n"
    "iCountry=1\r\nICurrDigits=2\r\niCurrency=0\r\niDate=0\r\niDigits=2\r\niLZero=0\r\n"
    "iMeasure=1\r\niNegCurr=0\r\niTime=0\r\niTLZero=0\r\ns1159=AM\r\ns2359=PM\r\nsCountry=United States\r\n"
    "sCurrency=$\r\nsDate=/\r\nsDecimal=.\r\nsLanguage=enu\r\nsList=,\r\nsLongDate=dddd, MMMM dd, yyyy\r\n"
    "sShortDate=M/d/yy\r\nsThousand=,\r\nsTime=:\r\n"
    "[fonts]\r\n"
    "CKFONT=CKFONT.FON\r\n"                     /* registered by the game's installer */
    "[mci extensions]\r\n"
    "wav=waveaudio\r\nmid=sequencer\r\nrmi=sequencer\r\navi=AVIVideo\r\n"
    "[sound]\r\n"
    "[devices]\r\n"
    "[PrinterPorts]\r\n";

typedef struct Ini {
    char path[128];              /* full guest path, upper case */
    char *text;
    int builtin;                 /* WIN.INI: kept in memory only */
} Ini;

#define MAX_INI 16
static Ini g_ini[MAX_INI];

static void ini_guest_path(const char *name, char *out, size_t cap)
{
    if (!strchr(name, '\\') && !strchr(name, ':') && !strchr(name, '/'))
        snprintf(out, cap, "C:\\WINDOWS\\%s", name);
    else
        vfs_full_path(name, out, cap);
    for (char *p = out; *p; p++)
        *p = (char)toupper((unsigned char)*p);
}

static Ini *ini_open(const char *name)
{
    char path[128];
    ini_guest_path(name, path, sizeof(path));
    int free_i = -1;
    for (int i = 0; i < MAX_INI; i++) {
        if (g_ini[i].text && !strcmp(g_ini[i].path, path))
            return &g_ini[i];
        if (!g_ini[i].text && free_i < 0)
            free_i = i;
    }
    if (free_i < 0)
        free_i = MAX_INI - 1;                     /* evict the last one */
    Ini *ini = &g_ini[free_i];
    free(ini->text);
    memset(ini, 0, sizeof(*ini));
    snprintf(ini->path, sizeof(ini->path), "%s", path);
    if (!strcmp(path, "C:\\WINDOWS\\WIN.INI")) {
        ini->text = strdup(g_win_ini_default);
        ini->builtin = 1;
        return ini;
    }
    char host[600];
    FILE *f = vfs_host_path(path, host, sizeof(host)) ? fopen(host, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        ini->text = malloc((size_t)len + 1);
        len = (long)fread(ini->text, 1, (size_t)len, f);
        ini->text[len] = 0;
        fclose(f);
    } else {
        ini->text = strdup("");
    }
    return ini;
}

static void ini_save(Ini *ini)
{
    if (ini->builtin)
        return;
    char host[600];
    FILE *f = vfs_host_path(ini->path, host, sizeof(host)) ? fopen(host, "wb") : NULL;
    if (!f) {
        LOGW("can't write profile %s", ini->path);
        return;
    }
    fwrite(ini->text, 1, strlen(ini->text), f);
    fclose(f);
}

static int ieq_n(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);
    if (an != bn)
        return 0;
    for (size_t i = 0; i < an; i++)
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return 0;
    return 1;
}

/* One line of a profile, split into its parts (pointers into the text). */
typedef struct IniLine {
    const char *start, *end;     /* whole line, without the line break */
    const char *next;            /* start of the following line */
    const char *name;            /* section name or key */
    size_t name_len;
    const char *value;
    size_t value_len;
    int kind;                    /* 0 other, 1 section, 2 key=value */
} IniLine;

static int ini_line(const char *p, IniLine *l)
{
    if (!*p)
        return 0;
    memset(l, 0, sizeof(*l));
    l->start = p;
    while (*p && *p != '\n' && *p != '\r')
        p++;
    l->end = p;
    if (*p == '\r')
        p++;
    if (*p == '\n')
        p++;
    l->next = p;
    const char *s = l->start, *e = l->end;
    while (s < e && (*s == ' ' || *s == '\t'))
        s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    if (s < e && *s == '[') {
        const char *close = memchr(s, ']', (size_t)(e - s));
        if (close) {
            l->kind = 1;
            l->name = s + 1;
            l->name_len = (size_t)(close - s - 1);
        }
    } else if (s < e && *s != ';') {
        const char *eq = memchr(s, '=', (size_t)(e - s));
        if (eq) {
            const char *ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t'))
                ke--;
            const char *v = eq + 1;
            while (v < e && (*v == ' ' || *v == '\t'))
                v++;
            l->kind = 2;
            l->name = s;
            l->name_len = (size_t)(ke - s);
            l->value = v;
            l->value_len = (size_t)(e - v);
        }
    }
    return 1;
}

/* Find a key in a section; returns the line, or 0. */
static int ini_find(const Ini *ini, const char *sect, const char *key, IniLine *out)
{
    int in = 0;
    IniLine l;
    for (const char *p = ini->text; ini_line(p, &l); p = l.next) {
        if (l.kind == 1)
            in = ieq_n(l.name, l.name_len, sect);
        else if (in && l.kind == 2 && ieq_n(l.name, l.name_len, key)) {
            *out = l;
            return 1;
        }
    }
    return 0;
}

/* Copy the value (or default) or the list of keys. Returns the count as Windows does. */
static uint16_t profile_get(const char *file, const char *sect, const char *key, const char *def, char *out,
                            uint16_t cap)
{
    if (!cap)
        return 0;
    Ini *ini = ini_open(file);
    if (!key) {                                    /* all keys of the section, double-NUL ended */
        size_t pos = 0;
        int in = 0;
        IniLine l;
        for (const char *p = ini->text; ini_line(p, &l); p = l.next) {
            if (l.kind == 1)
                in = sect ? ieq_n(l.name, l.name_len, sect) : 0;
            if ((sect ? (in && l.kind == 2) : l.kind == 1)) {
                if (pos + l.name_len + 2 > cap)
                    break;
                memcpy(out + pos, l.name, l.name_len);
                pos += l.name_len;
                out[pos++] = 0;
            }
        }
        out[pos] = 0;
        if (cap >= 2 && pos == 0)
            out[1] = 0;
        return (uint16_t)pos;
    }
    IniLine l;
    const char *src = def ? def : "";
    size_t len = strlen(src);
    if (ini_find(ini, sect, key, &l)) {
        src = l.value;
        len = l.value_len;
        if (len >= 2 && (src[0] == '"' || src[0] == '\'') && src[len - 1] == src[0]) {
            src++;
            len -= 2;
        }
    }
    if (len > (size_t)cap - 1)
        len = (size_t)cap - 1;
    memcpy(out, src, len);
    out[len] = 0;
    return (uint16_t)len;
}

static uint16_t profile_get_int(const char *file, const char *sect, const char *key, uint16_t def)
{
    char buf[32];
    if (!profile_get(file, sect, key, NULL, buf, sizeof(buf)))
        return def;
    return (uint16_t)strtol(buf, NULL, 10);
}

/* key NULL deletes the section, value NULL deletes the key. */
static int profile_write(const char *file, const char *sect, const char *key, const char *value)
{
    Ini *ini = ini_open(file);
    const char *t = ini->text;
    size_t tlen = strlen(t);
    /* locate the section (start of its header line, end of its last line) */
    const char *sec_start = NULL, *sec_end = NULL, *key_start = NULL, *key_end = NULL;
    int in = 0;
    IniLine l;
    for (const char *p = t; ini_line(p, &l); p = l.next) {
        if (l.kind == 1) {
            in = ieq_n(l.name, l.name_len, sect);
            if (in && !sec_start) {
                sec_start = l.start;
                sec_end = l.next;
            }
        } else if (in) {
            if (l.end > l.start)
                sec_end = l.next;
            if (l.kind == 2 && key && ieq_n(l.name, l.name_len, key) && !key_start) {
                key_start = l.start;
                key_end = l.next;
            }
        }
    }
    char line[600];
    const char *cut_s, *cut_e;
    line[0] = 0;
    if (!key) {                                    /* delete the whole section */
        if (!sec_start)
            return 1;
        cut_s = sec_start;
        cut_e = sec_end;
    } else if (key_start) {                        /* replace or delete the key */
        cut_s = key_start;
        cut_e = key_end;
        if (value)
            snprintf(line, sizeof(line), "%s=%s\r\n", key, value);
    } else {
        if (!value)
            return 1;
        if (sec_start) {                           /* append to the section */
            cut_s = cut_e = sec_end;
            snprintf(line, sizeof(line), "%s=%s\r\n", key, value);
        } else {                                   /* new section at the end */
            cut_s = cut_e = t + tlen;
            int nl = tlen && t[tlen - 1] != '\n';
            snprintf(line, sizeof(line), "%s[%s]\r\n%s=%s\r\n", nl ? "\r\n" : "", sect, key, value);
        }
    }
    size_t head = (size_t)(cut_s - t), tail = tlen - (size_t)(cut_e - t), mid = strlen(line);
    char *nt = malloc(head + mid + tail + 1);
    memcpy(nt, t, head);
    memcpy(nt + head, line, mid);
    memcpy(nt + head + mid, cut_e, tail);
    nt[head + mid + tail] = 0;
    free(ini->text);
    ini->text = nt;
    ini_save(ini);
    return 1;
}

static void GetProfileInt(CPU *c)      /* UINT GetProfileInt(LPCSTR app, LPCSTR key, int def) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "llw", a);
    c->a.x = profile_get_int("WIN.INI", guest_ptr(a[0]), guest_ptr(a[1]), (uint16_t)a[2]);
    LOGD("GetProfileInt(%s, %s) = %u", guest_ptr(a[0]), guest_ptr(a[1]), c->a.x);
    api_ret(c, n);
}

/* int GetProfileString(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, int size) */
static void GetProfileString(CPU *c)
{
    uint32_t a[5];
    uint16_t n = api_args(c, "llllw", a);
    c->a.x = profile_get("WIN.INI", guest_ptr(a[0]), guest_ptr(a[1]), guest_ptr(a[2]), guest_ptr(a[3]),
                         (uint16_t)a[4]);
    LOGD("GetProfileString(%s, %s) = \"%s\"", guest_ptr(a[0]), guest_ptr(a[1]) ? guest_ptr(a[1]) : "(all)",
         guest_ptr(a[3]));
    api_ret(c, n);
}

static void WriteProfileString(CPU *c)     /* BOOL WriteProfileString(LPCSTR, LPCSTR, LPCSTR) */
{
    uint32_t a[3];
    uint16_t n = api_args(c, "lll", a);
    c->a.x = (uint16_t)profile_write("WIN.INI", guest_ptr(a[0]), guest_ptr(a[1]), guest_ptr(a[2]));
    api_ret(c, n);
}

/* UINT GetPrivateProfileInt(LPCSTR app, LPCSTR key, int def, LPCSTR file) */
static void GetPrivateProfileInt(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llwl", a);
    c->a.x = profile_get_int(guest_ptr(a[3]), guest_ptr(a[0]), guest_ptr(a[1]), (uint16_t)a[2]);
    LOGD("GetPrivateProfileInt(%s, %s, %s) = %u", guest_ptr(a[3]), guest_ptr(a[0]), guest_ptr(a[1]), c->a.x);
    api_ret(c, n);
}

/* int GetPrivateProfileString(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, int size, LPCSTR file) */
static void GetPrivateProfileString(CPU *c)
{
    uint32_t a[6];
    uint16_t n = api_args(c, "llllwl", a);
    c->a.x = profile_get(guest_ptr(a[5]), guest_ptr(a[0]), guest_ptr(a[1]), guest_ptr(a[2]), guest_ptr(a[3]),
                         (uint16_t)a[4]);
    LOGD("GetPrivateProfileString(%s, %s, %s) = \"%s\"", guest_ptr(a[5]), guest_ptr(a[0]),
         guest_ptr(a[1]) ? guest_ptr(a[1]) : "(all)", guest_ptr(a[3]));
    api_ret(c, n);
}

/* BOOL WritePrivateProfileString(LPCSTR app, LPCSTR key, LPCSTR value, LPCSTR file) */
static void WritePrivateProfileString(CPU *c)
{
    uint32_t a[4];
    uint16_t n = api_args(c, "llll", a);
    c->a.x = (uint16_t)profile_write(guest_ptr(a[3]), guest_ptr(a[0]), guest_ptr(a[1]), guest_ptr(a[2]));
    api_ret(c, n);
}

const ApiDef g_kernel_file_api[] = {
    { "KERNEL", 57, "GetProfileInt", GetProfileInt },
    { "KERNEL", 58, "GetProfileString", GetProfileString },
    { "KERNEL", 59, "WriteProfileString", WriteProfileString },
    { "KERNEL", 74, "OpenFile", OpenFile },
    { "KERNEL", 81, "_lclose", lclose },
    { "KERNEL", 82, "_lread", lread },
    { "KERNEL", 83, "_lcreat", lcreat },
    { "KERNEL", 84, "_llseek", llseek },
    { "KERNEL", 85, "_lopen", lopen },
    { "KERNEL", 86, "_lwrite", lwrite },
    { "KERNEL", 127, "GetPrivateProfileInt", GetPrivateProfileInt },
    { "KERNEL", 128, "GetPrivateProfileString", GetPrivateProfileString },
    { "KERNEL", 129, "WritePrivateProfileString", WritePrivateProfileString },
    { "KERNEL", 134, "GetWindowsDirectory", GetWindowsDirectory },
    { "KERNEL", 135, "GetSystemDirectory", GetSystemDirectory },
    { "KERNEL", 136, "GetDriveType", GetDriveType },
    { NULL, 0, NULL, NULL }
};
