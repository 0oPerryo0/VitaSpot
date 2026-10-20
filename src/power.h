#ifndef VAM_POWER_H
#define VAM_POWER_H

/*
 * Screen power: dims and then turns off the screen after a while without
 * input, and turns it off on the screen-off button combo. Music keeps
 * playing; a button press turns the screen back on.
 */

#include <SDL.h>
#include <stdbool.h>

#include "platform/platform.h"

/* Applies the combo from prefs. */
void power_init(void);
/* Re-apply after the combo in prefs changed. */
void power_apply_prefs(void);

/* Every SDL event goes through here first. Returns true when the event only
 * woke the screen and must not reach the UI. */
bool power_handle_event(const SDL_Event *e);
/* Once per frame, after ui_begin_frame: combo and idle timers. */
void power_frame(void);
screen_level power_screen(void);

/* Choices offered in Settings, in seconds (0 = never). */
extern const int POWER_DIM_CHOICES[];
extern const int POWER_OFF_CHOICES[];
extern const int POWER_CHOICE_COUNT;

#endif
