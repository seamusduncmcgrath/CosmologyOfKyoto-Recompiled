/* Entry point: load the recompiled projector and start it like Windows 3.1 starts a task. */
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win.h"

extern const RecompModule play_e_module;
extern const RecompModule fileio_module;
extern const RecompModule macromix_module;

const KnownDll g_known_dlls[] = {
    { "FILEIO", "FILEIO.DLL", &fileio_module },
    { "MACROMIX", "MACROMIX.DLL", &macromix_module },     /* written to C:\WINDOWS by the projector */
    { NULL, NULL, NULL }
};

static CPU g_cpu_state;

/* A minimal DOS PSP and environment block: the C runtime startup reads the command line
 * (PSP:80h) and environment (selector at PSP:2Ch). */
static uint16_t make_psp(const char *program, const char *cmdline)
{
    uint16_t env = sel_alloc(0x200, SEL_DATA);
    uint8_t *e = g_seg_base[SEL_INDEX(env)];
    const char *vars[] = { "PATH=C:\\WINDOWS", "TEMP=C:\\TEMP", NULL };
    size_t p = 0;
    for (int i = 0; vars[i]; i++) {
        size_t n = strlen(vars[i]) + 1;
        memcpy(e + p, vars[i], n);
        p += n;
    }
    e[p++] = 0;                                    /* end of variables */
    e[p++] = 1; e[p++] = 0;                        /* one string follows: program path */
    memcpy(e + p, program, strlen(program) + 1);

    uint16_t psp = sel_alloc(0x100, SEL_DATA);
    uint8_t *ps = g_seg_base[SEL_INDEX(psp)];
    ps[0] = 0xCD; ps[1] = 0x20;                    /* int 20h */
    ps[0x2C] = (uint8_t)env; ps[0x2D] = (uint8_t)(env >> 8);
    size_t n = strlen(cmdline);
    if (n > 126)
        n = 126;
    ps[0x80] = (uint8_t)n;
    memcpy(ps + 0x81, cmdline, n);
    ps[0x81 + n] = '\r';
    return psp;
}

static void usage(void)
{
    fprintf(stderr, "usage: cok [-v] [--game DIR] [--fullscreen | --windowed] [PLAY_E.EXE [command line]]\n"
                    "  DIR holds the game: the contents of the Cosmology of Kyoto CD (default: the folder\n"
                    "  cok.exe is in, else the current directory). PLAY_E.EXE defaults to DIR/PLAY_E.EXE\n"
                    "  (the original, or one unpacked by tools/unpack.py).\n");
    exit(2);
}

static int file_exists(const char *dir, const char *name)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return SDL_GetPathInfo(path, NULL);
}

static void set_game_dir(const char *dir)
{
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
    size_t n = strlen(g_game_dir);
    while (n > 1 && (g_game_dir[n - 1] == '/' || g_game_dir[n - 1] == '\\'))
        g_game_dir[--n] = 0;
}

typedef struct Picked {
    SDL_AtomicInt done;
    char path[600];
} Picked;

static void SDLCALL folder_picked(void *userdata, const char *const *files, int filter)
{
    Picked *p = userdata;
    (void)filter;
    if (files && files[0])
        snprintf(p->path, sizeof(p->path), "%s", files[0]);
    SDL_SetAtomicInt(&p->done, 1);
}

/* The game folder (the CD contents): the program's own folder or one above it (cok.exe
 * dropped into the game folder; on macOS the app sits inside it), else the folder chosen
 * last time, else the current directory (on the Switch, the one cok.nro is in), else ask,
 * and remember the answer. */
static void find_game_dir(void)
{
    const char *base = SDL_GetBasePath();
    char dir[600];
    if (base) {
        snprintf(dir, sizeof(dir), "%s", base);
        for (int up = 0; up < 4 && dir[0]; up++) {          /* Contents/Resources -> .app -> its folder */
            if (file_exists(dir, "PLAY_E.EXE")) {
                set_game_dir(dir);
                return;
            }
            size_t n = strlen(dir);
            while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
                dir[--n] = 0;
            while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\')
                dir[--n] = 0;
        }
    }
    if (setting_str("game_dir", dir, sizeof(dir)) && file_exists(dir, "PLAY_E.EXE")) {
        set_game_dir(dir);
        return;
    }
    if (file_exists(".", "PLAY_E.EXE"))
        return;                                              /* g_game_dir is "." */
#ifdef __SWITCH__
    char *cwd = SDL_GetCurrentDirectory();                   /* no folder picker here */
    rt_fatal("Cosmology of Kyoto needs the game's files. Copy everything from the CD (PLAY_E.EXE, FILEIO.DLL, "
             "CKFONT.FON, the CK_DATA folder and so on) into %s, the folder cok.nro is in.",
             cwd ? cwd : "the folder cok.nro is in");
#endif
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    for (;;) {
        Picked p = { 0 };
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Cosmology of Kyoto",
                                 "Please choose the folder with the Cosmology of Kyoto game files: the contents "
                                 "of the CD (PLAY_E.EXE, FILEIO.DLL, CK_DATA and so on).", NULL);
        SDL_ShowOpenFolderDialog(folder_picked, &p, NULL, NULL, false);
        while (!SDL_GetAtomicInt(&p.done)) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (!p.path[0])
            rt_fatal("Cosmology of Kyoto needs the game's files (the contents of the CD) to run.");
        if (file_exists(p.path, "PLAY_E.EXE")) {
            set_game_dir(p.path);
            setting_set("game_dir", g_game_dir);
            return;
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Cosmology of Kyoto",
                                 "That folder doesn't contain PLAY_E.EXE.", NULL);
    }
}

int main(int argc, char **argv)
{
    int argi = 1, game_given = 0;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-v"))
            g_log_level = LOG_DEBUG;
        else if (!strcmp(argv[argi], "--game") && argi + 1 < argc)
            snprintf(g_game_dir, sizeof(g_game_dir), "%s", argv[++argi]), game_given = 1;
        else if (!strcmp(argv[argi], "--fullscreen"))
            g_start_fullscreen = 1;
        else if (!strcmp(argv[argi], "--windowed"))
            g_start_fullscreen = 0;
        else
            usage();
        argi++;
    }
    char exe_buf[700];
    const char *exe = argi < argc ? argv[argi++] : NULL;
    const char *cmdline = argi < argc ? argv[argi] : "";

    SDL_SetMainReady();
    host_init();
    char *pref = SDL_GetPrefPath(NULL, "CosmologyOfKyoto");   /* on the Switch, cok.nro's folder */
    if (pref) {
        rt_log_to_file_if_no_console(pref);
        snprintf(g_settings_path, sizeof(g_settings_path), "%ssettings.ini", pref);
    }
    crash_init();
    if (!game_given && !exe)
        find_game_dir();
    if (!exe) {
        snprintf(exe_buf, sizeof(exe_buf), "%s/PLAY_E.EXE", g_game_dir);
        exe = exe_buf;
    }
    /* C:\WINDOWS (profiles the program writes) lives in the per-user data directory */
    if (pref) {
        snprintf(g_win_dir, sizeof(g_win_dir), "%sWINDOWS", pref);
        SDL_CreateDirectory(g_win_dir);
        snprintf(g_save_dir, sizeof(g_save_dir), "%sSaves", pref);   /* C:\SAVES: file dialogs start here */
        SDL_CreateDirectory(g_save_dir);
        SDL_free(pref);
    }

    LoadedModule *m = ne_load(exe, &play_e_module);
    if (!m)
        return 1;
    snprintf(m->guest_path, sizeof(m->guest_path), "%s\\PLAY_E.EXE", GUEST_DIR);

    CPU *c = &g_cpu_state;
    cpu_reset(c);
    g_cpu = c;
    video_init();
    gdi_init();
    user_init();
    uint16_t dgroup = m->segsel[m->autodata];
    if (m->ss != m->autodata)
        rt_fatal("stack outside DGROUP isn't supported");
    /* DGROUP is [static data][stack][local heap]; SP 0 in the header means the top of the
     * stack area, i.e. data size + stack size (see Wine's NE_InitProcess) */
    uint32_t stack_top = (uint32_t)m->segs[m->autodata].minalloc + m->stack;

    /* registers at task start, as KERNEL sets them (see Wine's NE_StartTask) */
    c->a.x = 0;
    c->b.x = m->stack;
    c->c.x = m->heap;
    c->si.x = 0;                                   /* hPrevInstance */
    c->di.x = dgroup;                              /* hInstance */
    c->bp.x = 0;
    g_task = m;
    g_psp = make_psp(m->guest_path, cmdline);
    c->es = g_psp;
    c->ds = c->ss = dgroup;
    c->sp.x = (uint16_t)(m->sp ? m->sp : (stack_top >= 0x10000 ? 0xFFFE : stack_top));

    LOGI("starting %s at %s, SS:SP %04x:%04x", m->name, rt_where(m->segsel[m->cs], m->ip), c->ss, c->sp.x);
    rt_callback(c, m->segsel[m->cs], m->ip);
    LOGI("entry point returned");
    return 0;
}
