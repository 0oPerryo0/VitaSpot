#ifndef VAM_IMAGES_H
#define VAM_IMAGES_H

#include "gfx.h"
#include "vam_backend.h"

void images_init(void);
void images_shutdown(void);
/* Upload a couple of decoded images, queue downloads for the decode
 * thread, issue queued requests. */
void images_update(void);

/* Texture for url, or NULL while loading / on failure. Requests it if new.
 * avg (optional) receives a darkened dominant colour once loaded. */
/* Textures created by the last images_update, for diagnostics. */
int images_uploads_this_frame(void);

/* Cover art: square with slightly rounded corners baked in. */
SDL_Texture *images_get(const char *url, color *avg);
/* The same picture cut to a circle (artists, profile pictures). */
SDL_Texture *images_get_round(const char *url, color *avg);

/* Returns true if the event was an image reply this module consumed. */
bool images_on_event(const vam_event *ev);

#endif
