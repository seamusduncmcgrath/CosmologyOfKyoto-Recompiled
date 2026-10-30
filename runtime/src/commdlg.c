/* COMMDLG: the Open and Save As dialogs, shown as the host's native file dialogs (SDL3).
 *
 * The program gets a guest path back: files in the folders it already sees keep their
 * place (C:\COK, C:\WINDOWS, C:\SAVES), and any other folder the user picks is mounted as a
 * drive letter (vfs_guest_path). Dialogs start in C:\SAVES, the per-user saved games folder.
 * Hosts without file dialogs (the Switch) keep saved games in C:\SAVES and name them with
 * the system keyboard (prompt_file). Hooks and custom templates (OFN_ENABLEHOOK/TEMPLATE)
 * are ignored.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

/* OPENFILENAME (Win16) field offsets */
enum { OFN_FILTER = 8, OFN_FILTER_INDEX = 20, OFN_FILE = 24, OFN_MAX_FILE = 28, OFN_FILE_TITLE = 32,
       OFN_MAX_FILE_TITLE = 36, OFN_INITIAL_DIR = 40, OFN_TITLE = 44, OFN_FILE_OFFSET = 52, OFN_FILE_EXT = 54,
       OFN_DEF_EXT = 56 };
#define FNERR_BUFFERTOOSMALL 0x3003
#define MAX_FILTERS 16

static uint32_t g_ext_error;

typedef struct DialogResult {
    SDL_AtomicInt done;
    char *path;
    int filter;
} DialogResult;

/* may run on another thread */
static void SDLCALL dialog_done(void *userdata, const char *const *files, int filter)
{
    DialogResult *r = userdata;
    if (files && files[0])
        r->path = SDL_strdup(files[0]);
    else if (!files)
        LOGW("file dialog failed: %s", SDL_GetError());
    r->filter = filter;
    SDL_SetAtomicInt(&r->done, 1);
}

/* "Prefs (*.kpr)\0*.kpr\0All (*.*)\0*.*\0\0" -> SDL filters ("kpr", "*") */
static int read_filters(uint32_t far, SDL_DialogFileFilter *out, char store[][2][128])
{
    if (!far)
        return 0;
    const char *p = guest_ptr(far);
    int n = 0;
    while (*p && n < MAX_FILTERS) {
        const char *name = p;
        p += strlen(p) + 1;
        if (!*p)
            break;
        const char *spec = p;
        p += strlen(p) + 1;
        snprintf(store[n][0], 128, "%s", name);
        char *pat = store[n][1];
        size_t o = 0;
        int any = 0;
        for (const char *s = spec; *s;) {
            size_t len = strcspn(s, ";");
            const char *item = s;
            s += len + (s[len] == ';');
            while (len && *item == ' ')
                item++, len--;
            if (len >= 2 && item[0] == '*' && item[1] == '.')
                item += 2, len -= 2;
            if (!len || (len == 1 && item[0] == '*'))
                any = 1;
            else if (o + len + 2 < 128)
                o += (size_t)snprintf(pat + o, 128 - o, "%s%.*s", o ? ";" : "", (int)len, item);
        }
        if (any || !o)
            snprintf(pat, 128, "*");
        out[n].name = store[n][0];
        out[n].pattern = store[n][1];
        n++;
    }
    return n;
}

static int has_wildcard(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

typedef struct SaveFile {
    char name[64];
    int64_t mtime;
} SaveFile;

static int newest_first(const void *a, const void *b)
{
    int64_t ta = ((const SaveFile *)a)->mtime, tb = ((const SaveFile *)b)->mtime;
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

/* No file dialogs here: a saved game is a name, typed on the system keyboard, for a file in
 * C:\SAVES. Opening suggests the newest and lists the rest. Returns 1 with a host path in
 * *path, 0 if cancelled, -1 if there is no system keyboard either. */
static int prompt_file(int save, const char *suggest, const char *ext, char **path)
{
    char pattern[16], typed[64], sub[128] = "";
    snprintf(pattern, sizeof(pattern), "*.%s", ext);
    int n = 0;
    char **names = SDL_GlobDirectory(g_save_dir, pattern, SDL_GLOB_CASEINSENSITIVE, &n);
    SaveFile *files = n > 0 ? SDL_calloc((size_t)n, sizeof(*files)) : NULL;
    int nfiles = 0;
    for (int i = 0; files && i < n; i++) {
        char full[700];
        SDL_PathInfo info;
        snprintf(full, sizeof(full), "%s/%s", g_save_dir, names[i]);
        if (!SDL_GetPathInfo(full, &info) || info.type != SDL_PATHTYPE_FILE)
            continue;
        snprintf(files[nfiles].name, sizeof(files[nfiles].name), "%.*s",
                 (int)(strrchr(names[i], '.') ? strrchr(names[i], '.') - names[i] : (ptrdiff_t)strlen(names[i])),
                 names[i]);
        files[nfiles++].mtime = info.modify_time;
    }
    SDL_free(names);
    qsort(files, (size_t)nfiles, sizeof(*files), newest_first);
    for (int i = save ? 0 : 1; i < nfiles && strlen(sub) + strlen(files[i].name) + 3 < sizeof(sub); i++)
        snprintf(sub + strlen(sub), sizeof(sub) - strlen(sub), "%s%s", sub[0] ? ", " : save ? "Saved: " : "Also: ",
                 files[i].name);
    if (!save && !nfiles) {
        SDL_free(files);
        host_message(0, "There are no saved games yet.");
        return 0;
    }
    char initial[64];
    snprintf(initial, sizeof(initial), "%s", save ? suggest : files[0].name);
    char *dot = strchr(initial, '.');
    if (dot)
        *dot = 0;
    SDL_free(files);
    int r = host_text_prompt(save ? "Save the game as" : "Resume which saved game?", sub[0] ? sub : NULL, initial, 8,
                             typed, sizeof(typed));
    if (r <= 0)
        return r;
    for (char *c = typed; *c; c++)                /* a DOS name: letters, digits, - and _ */
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-'))
            *c = '_';
    char full[700];
    snprintf(full, sizeof(full), "%s/%s.%s", g_save_dir, typed, ext);
    if (!save && !SDL_GetPathInfo(full, NULL)) {
        host_message(0, "There is no saved game with that name.");
        return 0;
    }
    *path = SDL_strdup(full);
    return 1;
}

static int file_dialog(uint32_t ofn, int save)
{
    uint16_t s = FAR_SEL(ofn), o = FAR_OFF(ofn);
    uint32_t file_far = rd32(s, o + OFN_FILE), max_file = rd32(s, o + OFN_MAX_FILE);
    if (!file_far || !max_file)
        return 0;
    char *file = guest_ptr(file_far);
    uint32_t title_far = rd32(s, o + OFN_TITLE), defext_far = rd32(s, o + OFN_DEF_EXT);
    if (title_far && !*(const char *)guest_ptr(title_far))
        title_far = 0;                              /* empty: the host default ("Open", "Save As") */
    uint32_t initdir_far = rd32(s, o + OFN_INITIAL_DIR);

    /* where to start: the folder of a path given in lpstrFile, else C:\SAVES (the program's
     * own folder, C:\COK, is the CD's contents) */
    char guest_start[300], start[700] = "";
    const char *given = file[0] && !has_wildcard(file) ? file : "";
    const char *name = strrchr(given, '\\');
    name = name ? name + 1 : strchr(given, ':') ? strchr(given, ':') + 1 : given;
    if (name != given) {
        snprintf(guest_start, sizeof(guest_start), "%.*s", (int)(name - given), given);
    } else if (initdir_far && *(const char *)guest_ptr(initdir_far)) {
        vfs_full_path(guest_ptr(initdir_far), guest_start, sizeof(guest_start));
        if (!strncmp(guest_start, GUEST_DIR, strlen(GUEST_DIR)))
            guest_start[0] = 0;
    } else {
        guest_start[0] = 0;
    }
    if (!guest_start[0] || !vfs_host_path(guest_start, start, sizeof(start)))
        snprintf(start, sizeof(start), "%s", g_save_dir);
    if (save && *name) {                           /* suggest the file name */
        size_t len = strlen(start);
        snprintf(start + len, sizeof(start) - len, "%s%s", len && start[len - 1] != '/' ? "/" : "", name);
    } else if (start[0] && start[strlen(start) - 1] != '/') {
        strcat(start, "/");                         /* a folder, not a file named like it */
    }

    SDL_DialogFileFilter filters[MAX_FILTERS];
    char store[MAX_FILTERS][2][128];
    int nfilters = read_filters(rd32(s, o + OFN_FILTER), filters, store);
    LOGI("%s file dialog: start %s, %d filters%s%s", save ? "save" : "open", start, nfilters,
         title_far ? ", title " : "", title_far ? (const char *)guest_ptr(title_far) : "");

    DialogResult r = { 0 };
    r.filter = -1;
    const char *scripted = SDL_getenv("COK_FILE_DIALOG");   /* debugging: answer without asking */
    if (scripted)
        r.path = *scripted && strcmp(scripted, "cancel") ? SDL_strdup(scripted) : NULL;
    if (!scripted) {
        const char *ext = defext_far && *(const char *)guest_ptr(defext_far) ? guest_ptr(defext_far)
                          : nfilters && strcmp(filters[0].pattern, "*") ? filters[0].pattern : "SAV";
        char ext3[4];
        snprintf(ext3, sizeof(ext3), "%.3s", ext);
        if (prompt_file(save, *name ? name : "GAME", ext3, &r.path) >= 0)
            goto answered;
    }
    SDL_PropertiesID props = scripted ? 0 : SDL_CreateProperties();
    if (scripted)
        goto answered;
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, video_sdl_window());
    if (nfilters) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters);
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, nfilters);
    }
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, start);
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    if (title_far)
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, guest_ptr(title_far));
    video_native_modal(1);
    SDL_ShowFileDialogWithProperties(save ? SDL_FILEDIALOG_SAVEFILE : SDL_FILEDIALOG_OPENFILE, dialog_done, &r, props);
    while (!SDL_GetAtomicInt(&r.done)) {            /* modal: the program waits, the window lives */
        mm_poll();                                  /* and sound keeps playing */
        video_pump(5);
        video_present(0);
    }
    video_native_modal(0);
    SDL_DestroyProperties(props);
answered:
    if (!r.path)
        return 0;                                   /* cancelled */

    char guest[300];
    int ok = vfs_guest_path(r.path, guest, sizeof(guest));
    LOGI("file dialog: %s -> %s", r.path, ok ? guest : "(no guest path)");
    SDL_free(r.path);
    if (!ok)
        return 0;
    char *fname = strrchr(guest, '\\');
    fname = fname ? fname + 1 : guest;
    if (save && !strchr(fname, '.') && defext_far && *(const char *)guest_ptr(defext_far)) {
        size_t len = strlen(guest);
        snprintf(guest + len, sizeof(guest) - len, ".%.3s", (const char *)guest_ptr(defext_far));
    }
    size_t len = strlen(guest);
    if (len + 1 > max_file) {
        g_ext_error = FNERR_BUFFERTOOSMALL;
        if (max_file >= 2)
            wr16(FAR_SEL(file_far), FAR_OFF(file_far), (uint16_t)(len + 1));
        return 0;
    }
    memcpy(file, guest, len + 1);
    uint16_t name_off = (uint16_t)(fname - guest);
    const char *dot = strrchr(fname, '.');
    wr16(s, o + OFN_FILE_OFFSET, name_off);
    wr16(s, o + OFN_FILE_EXT, (uint16_t)(dot ? dot + 1 - guest : (ptrdiff_t)len));
    uint32_t title_buf = rd32(s, o + OFN_FILE_TITLE), max_title = rd32(s, o + OFN_MAX_FILE_TITLE);
    if (title_buf && max_title)
        snprintf(guest_ptr(title_buf), max_title, "%s", fname);
    if (r.filter >= 0)
        wr32(s, o + OFN_FILTER_INDEX, (uint32_t)r.filter + 1);
    return 1;
}

static void GetOpenFileName(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    g_ext_error = 0;
    c->a.x = (uint16_t)file_dialog(a[0], 0);
    api_ret(c, n);
}

static void GetSaveFileName(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    g_ext_error = 0;
    c->a.x = (uint16_t)file_dialog(a[0], 1);
    api_ret(c, n);
}

/* No printing: behave as if the user cancelled. */
static void PrintDlg(CPU *c)
{
    uint32_t a[1];
    uint16_t n = api_args(c, "l", a);
    c->a.x = 0;
    api_ret(c, n);
}

static void CommDlgExtendedError(CPU *c) { api_ret32(c, 0, g_ext_error); }

const ApiDef g_commdlg_api[] = {
    { "COMMDLG", 1, "GetOpenFileName", GetOpenFileName },
    { "COMMDLG", 2, "GetSaveFileName", GetSaveFileName },
    { "COMMDLG", 20, "PrintDlg", PrintDlg },
    { "COMMDLG", 26, "CommDlgExtendedError", CommDlgExtendedError },
    { NULL, 0, NULL, NULL }
};
