#include "power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"

#define CAPTURE_TIMEOUT_MS 8000
/* The Vita's stick reports small drift; only a real push counts as input. */
#define STICK_DEADZONE 8000

const int POWER_DIM_CHOICES[] = { 0, 15, 30, 60, 120, 300 };
const int POWER_OFF_CHOICES[] = { 0, 30, 60, 120, 300, 600 };
const int POWER_CHOICE_COUNT = 6;

static power_settings g_set = { { BTN_SELECT, BTN_START }, 30, 120 };
static screen_level g_level = SCREEN_ON;
static Uint64 g_last_input;
static Uint64 g_capture_since;

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

static int seconds(const char *v)
{
    int s = atoi(v);
    return s < 0 ? 0 : s;
}

void power_init(void)
{
    char path[512];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
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
                        g_set.combo[0] = (btn)x;
                        g_set.combo[1] = (btn)y;
                    }
                }
            } else if (strcmp(line, "dim_after") == 0) {
                g_set.dim_after = seconds(v);
            } else if (strcmp(line, "screen_off_after") == 0) {
                g_set.off_after = seconds(v);
            }
        }
        fclose(f);
    }
    ui_set_combo(g_set.combo[0], g_set.combo[1]);
    g_last_input = SDL_GetTicks64();
}

power_settings *power_settings_get(void) { return &g_set; }

void power_settings_save(void)
{
    ui_set_combo(g_set.combo[0], g_set.combo[1]);
    char path[512];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "screen_off_combo=%s+%s\n", KEYS[g_set.combo[0]], KEYS[g_set.combo[1]]);
    fprintf(f, "dim_after=%d\n", g_set.dim_after);
    fprintf(f, "screen_off_after=%d\n", g_set.off_after);
    fclose(f);
}

screen_level power_screen(void) { return g_level; }

static void set_level(screen_level level)
{
    if (level == g_level)
        return;
    g_level = level;
    platform_set_screen(level);
}

bool power_handle_event(const SDL_Event *e)
{
    bool button = false;
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_JOYBUTTONDOWN:
        button = true;
        break;
    case SDL_JOYAXISMOTION:
        if (abs(e->jaxis.value) < STICK_DEADZONE)
            return g_level == SCREEN_OFF;
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (abs(e->caxis.value) < STICK_DEADZONE)
            return g_level == SCREEN_OFF;
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
        break;
    default:
        return false;
    }
    /* With the screen off only a button wakes it (not a touch in a pocket),
       and that press does nothing else. */
    if (g_level == SCREEN_OFF) {
        if (button) {
            g_last_input = SDL_GetTicks64();
            set_level(SCREEN_ON);
        }
        return true;
    }
    g_last_input = SDL_GetTicks64();
    set_level(SCREEN_ON);
    return false;
}

void power_frame(void)
{
    Uint64 now = SDL_GetTicks64();
    if (ui_capturing_combo()) {
        /* Rebinding in Settings: stay awake, and give up after a while. */
        if (!g_capture_since)
            g_capture_since = now;
        else if (now - g_capture_since > CAPTURE_TIMEOUT_MS)
            ui_capture_combo(false);
        g_last_input = now;
        return;
    }
    g_capture_since = 0;
    if (platform_take_wake()) {
        /* The system already restored the backlight. */
        g_level = SCREEN_ON;
        g_last_input = now;
    }
    if (ui_combo_fired()) {
        set_level(SCREEN_OFF);
        return;
    }
    Uint64 idle = now - g_last_input;
    if (g_set.off_after > 0 && idle >= (Uint64)g_set.off_after * 1000)
        set_level(SCREEN_OFF);
    else if (g_set.dim_after > 0 && idle >= (Uint64)g_set.dim_after * 1000 && g_level == SCREEN_ON)
        set_level(SCREEN_DIM);
}
