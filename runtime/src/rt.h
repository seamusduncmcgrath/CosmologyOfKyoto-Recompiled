/* Runtime-internal declarations (not used by generated code). */
#ifndef RT_H
#define RT_H

#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

/* ---- logging ------------------------------------------------------------------------ */

enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };
extern int g_log_level;
void rt_log(int level, const char *fmt, ...);
#define LOGE(...) rt_log(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) rt_log(LOG_WARN, __VA_ARGS__)
#define LOGI(...) rt_log(LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (g_log_level >= LOG_DEBUG) rt_log(LOG_DEBUG, __VA_ARGS__); } while (0)
RT_NORETURN void rt_fatal(const char *fmt, ...);

/* ---- selectors (Windows 3.1 style LDT) ------------------------------------------------ */

#define SEL_COUNT 8192
#define SEL_INDEX(sel) ((sel) >> 3)
#define SEL_MAKE(index) ((uint16_t)((index) << 3 | 7))     /* LDT, RPL 3, as in Win16 */

enum { SEL_FREE, SEL_DATA, SEL_CODE };

typedef struct Selector {
    uint8_t kind;
    uint32_t limit;              /* last valid offset */
    uint8_t *alloc;              /* start of the host allocation (first selector of a block) */
    uint16_t count;              /* number of selectors in the block (first selector only) */
    struct LoadedModule *module; /* owning module for code/data segments, else NULL */
    uint16_t segnum;             /* NE segment number within module */
    uint32_t linear;             /* linear base address, as GetSelectorBase reports it */
    uint8_t alias;               /* made by AllocSelector: points into another block */
} Selector;

extern Selector g_sel[SEL_COUNT];

/* Allocate `size` bytes (zeroed) mapped by consecutive selectors 64 KB apart, so that
 * huge-pointer arithmetic (selector += __AHINCR) walks the block. Returns the first
 * selector, or 0. */
uint16_t sel_alloc(uint32_t size, int kind);
void sel_free(uint16_t sel);
int sel_valid(uint16_t sel);
uint16_t sel_table(void);        /* KERNEL's selector table (for THHOOK) */
extern int g_dib_shadows_pending;
void gdi_flush_dib_shadows(void); /* after each API call (gdi.c) */
void mm_poll(void);              /* wave.c: deliver due audio/timer callbacks */
int mm_wait_ms(int wait_ms);     /* longest sleep that keeps them on time */
uint32_t sel_size(uint16_t sel);           /* bytes in the block from this selector on */
/* AllocSelector: a selector copying `src`'s base and limit (or empty for 0), owning no
 * memory; sel_set_base points it at a linear address inside an allocated block. */
uint16_t sel_alloc_alias(uint16_t src, int as_data);
int sel_free_alias(uint16_t sel);
int sel_set_base(uint16_t sel, uint32_t linear);

/* ---- loaded NE modules -------------------------------------------------------------- */

typedef struct NESeg {
    uint16_t sector, length, flags, minalloc;
} NESeg;

typedef struct LoadedModule {
    const RecompModule *rm;      /* generated code for this module, or NULL */
    char path[512];
    char name[16];               /* module name (resident name table) */
    uint8_t *file;
    size_t file_size;
    uint16_t ne;                 /* offset of the NE header */
    uint16_t flags, autodata, heap, stack;
    uint16_t cs, ip, ss, sp;     /* from the header (segment numbers) */
    uint16_t nseg;
    NESeg *segs;
    uint16_t *segsel;            /* [segnum] -> selector */
    uint16_t nentries;
    struct { uint16_t seg, off; } *entries;    /* [ordinal] */
    uint16_t hmodule;            /* handle given to the program (= DGROUP selector or a fake) */
    char guest_path[128];        /* path the program sees, e.g. C:\COK\FILEIO.DLL */
} LoadedModule;

LoadedModule *ne_load(const char *path, const RecompModule *rm);
int optloader_unpack(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n, char *err, size_t errcap);
LoadedModule *module_by_name(const char *name);
LoadedModule *module_by_handle(uint16_t h);    /* hModule, hInstance or any segment selector */
/* Recompiled DLLs available to LoadLibrary (main.c). */
typedef struct KnownDll {
    const char *name;            /* module name, e.g. "FILEIO" */
    const char *file;            /* file in the game directory */
    const RecompModule *rm;
} KnownDll;
extern const KnownDll g_known_dlls[];
int module_entry(LoadedModule *m, uint16_t ordinal, uint16_t *sel, uint16_t *off);
int module_export_by_name(LoadedModule *m, const char *name, uint16_t *ordinal);

/* ---- Win16 API slots (imports and their thunk addresses) ------------------------------- */

typedef struct ApiDef {
    const char *module;
    uint16_t ordinal;
    const char *name;
    RecompFn fn;
} ApiDef;

extern uint16_t g_thunk_sel;

/* Slot for an imported function; created on first use. */
int api_slot(const char *module, uint16_t ordinal, const char *name, const char *label);
uint32_t api_slot_addr(int slot);          /* far address of its thunk */
/* Absolute-value exports (KERNEL __AHINCR etc.): returns 1 and the value if constant. */
int api_constant(const char *module, uint16_t ordinal, uint16_t *value);
void api_call_slot(CPU *c, int slot);

/* ---- writing Win16 API functions ------------------------------------------------------ *
 *
 * APIs use the Pascal convention: arguments pushed left to right, callee pops. On entry the
 * stack holds the far return address (4 bytes) and then the arguments, last one first.
 * api_args reads them given a signature in declaration order ('w' 16-bit, 'l' 32-bit or
 * far pointer) and returns their total size for api_ret.
 */
uint16_t api_args(CPU *c, const char *sig, uint32_t *out);
static inline void api_ret(CPU *c, uint16_t argbytes) { c->sp.x += 4 + argbytes; }
static inline void api_ret32(CPU *c, uint16_t argbytes, uint32_t v)
{
    c->a.x = (uint16_t)v;
    c->d.x = (uint16_t)(v >> 16);
    api_ret(c, argbytes);
}
#define FAR_SEL(p) ((uint16_t)((p) >> 16))
#define FAR_OFF(p) ((uint16_t)(p))
/* Host view of a guest far pointer (NULL for a null pointer). */
static inline char *guest_ptr(uint32_t far) { return far ? (char *)mem_ptr(FAR_SEL(far), FAR_OFF(far)) : NULL; }

/* Create a local heap in segment `sel` starting at `start` (kernel_mem.c). */
int local_init(uint16_t sel, uint16_t start, uint16_t end);

/* Global heap blocks, as GlobalAlloc/GlobalFree; global_sel turns a handle into its selector. */
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_DISCARDABLE 0x0100
uint16_t global_alloc(uint16_t flags, uint32_t size);
int global_free(uint16_t h);
uint16_t global_sel(uint16_t h);

/* ---- resources (kernel_res.c) ------------------------------------------------------------- */

/* A resource type or name: an integer id, or a string (name != NULL). */
typedef struct ResId {
    uint16_t id;
    const char *name;
} ResId;
enum { RT_CURSOR = 1, RT_BITMAP, RT_ICON, RT_MENU, RT_DIALOG, RT_STRING, RT_FONTDIR, RT_FONT,
       RT_ACCELERATOR, RT_RCDATA, RT_GROUP_CURSOR = 12, RT_GROUP_ICON = 14 };
static inline ResId res_int(uint16_t id) { ResId r = { id, NULL }; return r; }
/* Decode a guest LPCSTR resource argument: MAKEINTRESOURCE, "#123" or a name. */
ResId res_id_guest(uint32_t far);
/* Find a resource's data in a module (NULL if absent). */
const uint8_t *res_find(struct LoadedModule *m, ResId type, ResId name, uint32_t *size);

/* ---- the running task ----------------------------------------------------------------- */

extern uint16_t g_psp;           /* PSP selector of the task */
extern LoadedModule *g_task;     /* the task's EXE module */
extern uint16_t g_cmd_show;

/* ---- DOS and the guest file system ------------------------------------------------------ */

extern char g_game_dir[512];     /* host directory holding the CD contents */
extern char g_settings_path[600]; /* our own settings (settings.c), in the per-user folder */
int setting_str(const char *key, char *out, size_t cap);
int setting_int(const char *key, int def);
void setting_set(const char *key, const char *value);
extern int g_start_fullscreen;   /* -1: as last time; 0/1 from the command line */
extern char g_win_dir[512];      /* writable host directory standing in for C:\WINDOWS */
extern char g_save_dir[512];     /* host directory standing in for C:\SAVES (saved games) */
int vfs_guest_path(const char *host, char *out, size_t cap);
int vfs_drive_mounted(int letter);
#define GUEST_DIR "C:\\COK"      /* where the program believes it lives */
#define GUEST_CD_DRIVE 'D'       /* the CD-ROM drive; also maps to g_game_dir */
#define GUEST_CD_DRIVE_TYPE 4    /* GetDriveType: CD-ROMs report DRIVE_REMOTE under Win16 */
/* Absolute, upper-case guest path with "." and ".." resolved. */
void vfs_full_path(const char *guest, char *out, size_t cap);
/* Map a guest DOS path to a host path. Returns 0 if it can't be mapped. */
int vfs_host_path(const char *guest, char *out, size_t cap);
void dos_int21(CPU *c);
/* DOS file handles, shared by int 21h and KERNEL's _lopen/_lread/...; negative = DOS error.
 * mode: 0 read, 1 write, 2 read/write, 3 create. */
int dos_open(const char *guest, int mode);
int dos_close(uint16_t h);
long dos_read(uint16_t h, void *buf, uint16_t n);
long dos_write(uint16_t h, const void *buf, uint16_t n);
long dos_seek(uint16_t h, int32_t off, int origin);

/* ---- CPU / execution ---------------------------------------------------------------- */

void cpu_reset(CPU *c);
/* Milliseconds since start (the clock behind GetTickCount and timeGetTime). */
uint32_t rt_ticks(void);
/* Let the host window system breathe (KERNEL Yield and friends; user_msg.c). */
void user_yield(void);
/* Call a far address as the Win16 kernel would: pushes a return address into a thunk
 * that marks the end of the call, then dispatches. */
void rt_callback(CPU *c, uint16_t sel, uint16_t off);
extern uint8_t *g_watch_lo, *g_watch_hi;  /* RT_WATCH watchpoint range (crash.c) */
/* Log the guest call chain (BP frames) for debugging. */
void rt_backtrace(CPU *c);
/* Map a selector:offset to "module:seg:off" text for logs. */
const char *rt_where(uint16_t sel, uint16_t off);

#endif
