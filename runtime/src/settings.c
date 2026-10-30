/* Our own settings (not the game's): settings.ini in the per-user folder, "key=value" lines.
 * Read and written whole; there are only a few. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

char g_settings_path[600];

#define MAX_SETTINGS 16

typedef struct Setting {
    char key[32];
    char value[600];
} Setting;

static int load(Setting *s)
{
    FILE *f = g_settings_path[0] ? fopen(g_settings_path, "r") : NULL;
    int n = 0;
    char line[700];
    while (f && n < MAX_SETTINGS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == ';' || !eq)
            continue;
        *eq = 0;
        snprintf(s[n].key, sizeof(s[n].key), "%s", line);
        snprintf(s[n].value, sizeof(s[n].value), "%s", eq + 1);
        n++;
    }
    if (f)
        fclose(f);
    return n;
}

int setting_str(const char *key, char *out, size_t cap)
{
    Setting s[MAX_SETTINGS];
    int n = load(s);
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].key, key)) {
            snprintf(out, cap, "%s", s[i].value);
            return 1;
        }
    return 0;
}

int setting_int(const char *key, int def)
{
    char v[32];
    return setting_str(key, v, sizeof(v)) ? atoi(v) : def;
}

void setting_set(const char *key, const char *value)
{
    Setting s[MAX_SETTINGS];
    int n = load(s), i = 0;
    while (i < n && strcmp(s[i].key, key))
        i++;
    if (i == n) {
        if (n == MAX_SETTINGS)
            return;
        n++;
        snprintf(s[i].key, sizeof(s[i].key), "%s", key);
    }
    snprintf(s[i].value, sizeof(s[i].value), "%s", value);
    FILE *f = g_settings_path[0] ? fopen(g_settings_path, "w") : NULL;
    if (!f)
        return;
    fputs("; Cosmology of Kyoto (recompiled) settings\n", f);
    for (i = 0; i < n; i++)
        fprintf(f, "%s=%s\n", s[i].key, s[i].value);
    fclose(f);
}
