#include "prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

/* How long prefs must stay unchanged before they are written. */
#define SAVE_DELAY_MS 1000

static prefs g_prefs;
static bool g_dirty;
static Uint64 g_changed_at;

/* Names in settings.txt; stable even if the btn enum is reordered. */
static const char *const KEYS[BTN_COUNT] = { "up", "down", "left", "right", "confirm", "back",
                                             "triangle", "square", "l", "r", "start", "select" };

static int key_btn(const char *s)
{
    for (int i = 0; i < BTN_COUNT; i++)
        if (strcmp(KEYS[i], s) == 0)
            return i;
    return -1;
}

static void settings_path(char *buf, size_t n) { snprintf(buf, n, "%ssettings.txt", platform_data_dir()); }

/* The new file is written here first, then renamed over settings.txt. */
static void temp_path(char *buf, size_t n) { snprintf(buf, n, "%ssettings.txt.tmp", platform_data_dir()); }

static int seconds(const char *v)
{
    int s = atoi(v);
    return s < 0 ? 0 : s;
}

void prefs_load(void)
{
    g_prefs.combo[0] = BTN_SELECT;
    g_prefs.combo[1] = BTN_START;
    g_prefs.dim_after = 30;
    g_prefs.off_after = 120;
    g_prefs.bitrate = platform_is_vita() ? 320 : 160;

    char path[512];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        /* A save that stopped between removing settings.txt and renaming the
           finished temp file over it leaves only the temp file. */
        temp_path(path, sizeof(path));
        f = fopen(path, "r");
    }
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        if (strcmp(line, "screen_off_combo") == 0) {
            char a[16], b[16];
            if (sscanf(v, "%15[^+]+%15s", a, b) == 2) {
                int x = key_btn(a), y = key_btn(b);
                if (x >= 0 && y >= 0 && x != y) {
                    g_prefs.combo[0] = (btn)x;
                    g_prefs.combo[1] = (btn)y;
                }
            }
        } else if (strcmp(line, "dim_after") == 0) {
            g_prefs.dim_after = seconds(v);
        } else if (strcmp(line, "screen_off_after") == 0) {
            g_prefs.off_after = seconds(v);
        } else if (strcmp(line, "audio_quality") == 0) {
            int k = atoi(v);
            if (k == 96 || k == 160 || k == 320)
                g_prefs.bitrate = k;
        } else if (strcmp(line, "lyrics") == 0) {
            g_prefs.lyrics = atoi(v) != 0;
        }
    }
    fclose(f);
}

prefs *prefs_get(void) { return &g_prefs; }

/* Writes a complete temp file, then swaps it in, so a crash or power-off
   mid-save never leaves a truncated settings.txt behind. */
static void save(void)
{
    char path[512], tmp[512];
    settings_path(path, sizeof(path));
    temp_path(tmp, sizeof(tmp));
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "screen_off_combo=%s+%s\n", KEYS[g_prefs.combo[0]], KEYS[g_prefs.combo[1]]);
    fprintf(f, "dim_after=%d\n", g_prefs.dim_after);
    fprintf(f, "screen_off_after=%d\n", g_prefs.off_after);
    fprintf(f, "audio_quality=%d\n", g_prefs.bitrate);
    fprintf(f, "lyrics=%d\n", g_prefs.lyrics ? 1 : 0);
    bool ok = !ferror(f);
    if (fclose(f) != 0 || !ok) {
        remove(tmp);
        return;
    }
    /* The Vita and Windows refuse to rename over an existing file. */
    if (rename(tmp, path) != 0) {
        remove(path);
        rename(tmp, path);
    }
}

void prefs_changed(void)
{
    g_dirty = true;
    g_changed_at = SDL_GetTicks64();
}

void prefs_frame(void)
{
    if (g_dirty && SDL_GetTicks64() - g_changed_at >= SAVE_DELAY_MS)
        prefs_flush();
}

void prefs_flush(void)
{
    if (!g_dirty)
        return;
    g_dirty = false;
    save();
}
