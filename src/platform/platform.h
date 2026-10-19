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

/* Called once per frame. `keep_awake` stops auto-suspend while audio plays. */
void platform_frame(bool keep_awake);

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
