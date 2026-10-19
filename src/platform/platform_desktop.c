#ifndef __vita__

#include "platform.h"

#include <SDL.h>
#include <stdio.h>

static char g_base[1024];
static char g_data[1024];

void platform_init(void)
{
    char *base = SDL_GetBasePath();
    snprintf(g_base, sizeof(g_base), "%s", base ? base : "./");
    SDL_free(base);

    /* VAM_DATA_DIR overrides the data folder, e.g. for scripted test runs. */
    const char *override = SDL_getenv("VAM_DATA_DIR");
    if (override && override[0]) {
        snprintf(g_data, sizeof(g_data), "%s", override);
        return;
    }
    char *pref = SDL_GetPrefPath("VitaSpot", "VitaSpot");
    snprintf(g_data, sizeof(g_data), "%s", pref ? pref : "./");
    SDL_free(pref);
}

void platform_shutdown(void) {}

void platform_frame(bool keep_awake) { (void)keep_awake; }

bool platform_is_vita(void) { return false; }

void platform_lower_thread_priority(void) { SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW); }

const char *platform_asset_path(const char *rel, char *buf, size_t size)
{
    snprintf(buf, size, "%sassets/%s", g_base, rel);
    return buf;
}

const char *platform_data_dir(void) { return g_data; }

int platform_map_vita_button(int b)
{
    (void)b;
    return -1;
}

#endif
