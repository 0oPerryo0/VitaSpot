#include "power.h"

#include <stdlib.h>

#include "prefs.h"
#include "ui/ui.h"

#define CAPTURE_TIMEOUT_MS 8000
/* The Vita's stick reports small drift; only a real push counts as input. */
#define STICK_DEADZONE 8000

const int POWER_DIM_CHOICES[] = { 0, 15, 30, 60, 120, 300 };
const int POWER_OFF_CHOICES[] = { 0, 30, 60, 120, 300, 600 };
const int POWER_CHOICE_COUNT = 6;

static screen_level g_level = SCREEN_ON;
static Uint64 g_last_input;
static Uint64 g_capture_since;

void power_init(void)
{
    power_apply_prefs();
    g_last_input = SDL_GetTicks64();
}

void power_apply_prefs(void)
{
    prefs *p = prefs_get();
    ui_set_combo(p->combo[0], p->combo[1]);
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
    const prefs *p = prefs_get();
    if (p->off_after > 0 && idle >= (Uint64)p->off_after * 1000)
        set_level(SCREEN_OFF);
    else if (p->dim_after > 0 && idle >= (Uint64)p->dim_after * 1000 && g_level == SCREEN_ON)
        set_level(SCREEN_DIM);
}
