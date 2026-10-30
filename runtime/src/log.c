#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "win.h"

int g_log_level = LOG_INFO;

static const char *const level_names[] = { "error", "warn", "info", "debug" };

void rt_log(int level, const char *fmt, ...)
{
    if (level > g_log_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", level_names[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[fatal] %s\n", msg);
    fflush(stderr);
    host_message(1, msg);
    exit(1);
}
