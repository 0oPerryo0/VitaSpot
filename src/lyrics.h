#ifndef VAM_LYRICS_H
#define VAM_LYRICS_H

/*
 * Lyrics of the playing track for Now Playing. One track at a time: the
 * lyrics are fetched when the track changes (never while the screen is off)
 * and dropped on the next change.
 */

#include <stdbool.h>

#include "gfx/gfx.h"
#include "vam_backend.h"

/* Once per frame, also while the screen is off. */
void lyrics_frame(void);
void lyrics_on_event(const vam_event *ev);
/* Forget everything (logout). */
void lyrics_reset(void);
/* The playing track's lyrics have arrived. False while loading and for
   tracks without lyrics. */
bool lyrics_available(void);
/* Draws the lyrics column into area; alpha fades it in. */
void lyrics_draw(rectf area, float alpha);

#endif
