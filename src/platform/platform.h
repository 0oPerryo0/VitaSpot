#ifndef VAM_PLATFORM_H
#define VAM_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* Logical buttons. On desktop these come from keyboard / game controller. */
typedef enum btn {
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_CONFIRM,   /* Cross (Circle on Japanese/Asian units)  */
    BTN_BACK,      /* Circle (Cross on Japanese/Asian units)  */
    BTN_TRIANGLE,  /* play / pause                            */
    BTN_SQUARE,    /* item menu                               */
    BTN_L,         /* previous track                          */
    BTN_R,         /* next track                              */
    BTN_START,     /* toggle Now Playing                      */
    BTN_SELECT,    /* queue                                   */
    BTN_COUNT
} btn;

void platform_init(void);
void platform_shutdown(void);

/* Called once per frame. `keep_awake` stops auto-suspend while audio plays.
 * The system's own screen dimming is held off: the app dims it (power.c). */
void platform_frame(bool keep_awake);

typedef enum screen_level { SCREEN_ON, SCREEN_DIM, SCREEN_OFF } screen_level;
/* Backlight: full (the user's brightness setting), dimmed, or off. Desktop
 * has no backlight; the app shades the window instead. */
void platform_set_screen(screen_level level);
/* True once after the system put the screen back on by itself (the PS or
 * power button, suspend, resume), so the app's screen state can follow. */
bool platform_take_wake(void);

/* Physical button name ("Cross", "L", "Start"...), following the console's
 * Enter-button setting for Confirm / Back. */
const char *platform_btn_label(btn b);

bool platform_is_vita(void);

/* Call from a worker thread so its work never preempts rendering. */
void platform_lower_thread_priority(void);

/* Path to a read-only bundled asset: "app0:assets/<rel>" on Vita. */
const char *platform_asset_path(const char *rel, char *buf, size_t size);

/* Writable directory with trailing slash, created on first call. */
const char *platform_data_dir(void);

/* Map a Vita SDL joystick button index to a logical button, or -1. */
int platform_map_vita_button(int sdl_button);

#endif
