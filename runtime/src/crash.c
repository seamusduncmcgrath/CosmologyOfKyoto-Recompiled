/* Crash reports for development: on a fault, print the native stack (symbolized from the
 * PDB on Windows) and the guest call chain, so a bad memory access in recompiled code or
 * the runtime can be traced back to the guest instruction stream. */
#include <stdio.h>

#include "win.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    static int busy;
    if (busy)
        return EXCEPTION_CONTINUE_SEARCH;
    busy = 1;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "[fatal] exception %08lx at %p", er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " (%s %p)", er->ExceptionInformation[0] ? "writing" : "reading",
                (void *)er->ExceptionInformation[1]);
    fputc('\n', stderr);

#if defined(_M_X64) || defined(__x86_64__)        /* the native stack walk is x64-specific */
    HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f = { 0 };
    f.AddrPC.Offset = ctx.Rip;
    f.AddrFrame.Offset = ctx.Rbp;
    f.AddrStack.Offset = ctx.Rsp;
    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &ctx, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL) || !f.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        int have_sym = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym);
        int have_line = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line);
        fprintf(stderr, "  %2d %s+0x%llx", i, have_sym ? sym->Name : "?", (unsigned long long)disp);
        if (have_line)
            fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
        fputc('\n', stderr);
    }
#endif
    if (g_cpu) {
        fprintf(stderr, "guest: CS:IP unknown, SS:SP %04x:%04x BP %04x DS %04x ES %04x\n", g_cpu->ss, g_cpu->sp.x,
                g_cpu->bp.x, g_cpu->ds, g_cpu->es);
        rt_backtrace(g_cpu);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void crash_init(void)
{
    SetUnhandledExceptionFilter(on_crash);
}

/* A release build (GUI subsystem) has no console: keep the log, crash reports included, in
 * the per-user folder for bug reports. Run from a terminal, a console build logs there. */
void rt_log_to_file_if_no_console(const char *pref)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE)
        return;
    char path[600];
    snprintf(path, sizeof(path), "%scok.log", pref);
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}

void rt_native_backtrace(int max)
{
    HANDLE proc = GetCurrentProcess();
    static int sym_init;
    if (!sym_init) {
        sym_init = 1;
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(proc, NULL, TRUE);
    }
    void *frames[32];
    USHORT n = CaptureStackBackTrace(1, max < 32 ? max : 32, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        fprintf(stderr, "  %2u %s+0x%llx\n", i, SymFromAddr(proc, (DWORD64)frames[i], &disp, sym) ? sym->Name : "?",
                (unsigned long long)disp);
    }
}
#else
#include <unistd.h>

void crash_init(void)
{
}

void rt_native_backtrace(int max)
{
    (void)max;
}

/* Started from the Finder (or a console's launcher), stderr goes nowhere useful: keep the
 * log in the per-user folder instead. From a terminal (or nxlink), it stays there. */
void rt_log_to_file_if_no_console(const char *pref)
{
    if (isatty(2) || host_has_console())
        return;
    char path[600];
    snprintf(path, sizeof(path), "%scok.log", pref);
    if (freopen(path, "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}
#endif

/* ---- watchpoints (RT_WATCH builds) --------------------------------------------------------- */

uint8_t *g_watch_lo, *g_watch_hi;

void rt_watch_hit(uint8_t *p, int size, uint32_t value)
{
    static unsigned long hits, nonzero;
    static long long lo = -1, hi = -1;
    hits++;
    if (value)
        nonzero++;
    long long off = (long long)(p - g_watch_lo);
    if (hits > 307200) {                    /* after the first full-screen erase */
        if (lo < 0 || off < lo) lo = off;
        if (off > hi) hi = off;
    }
    if ((hits & (hits - 1)) == 0)
        fprintf(stderr, "[watch] %lu writes so far, %lu nonzero; after the erase: offsets %lld..%lld\n", hits, nonzero,
                lo, hi);
    if (hits == 307201 || hits == 400000) {
        fprintf(stderr, "[watch] write #%lu at offset %lld (row %lld from the top)\n", hits, off, off / 640);
        rt_native_backtrace(12);
        if (g_cpu)
            rt_backtrace(g_cpu);
    }
    if (!value || nonzero > 3)
        return;
    fprintf(stderr, "[watch] write of %d bytes, value %x, at offset %lld of the watched range\n", size, value,
            (long long)(p - g_watch_lo));
    rt_native_backtrace(12);
    if (g_cpu)
        rt_backtrace(g_cpu);
}
