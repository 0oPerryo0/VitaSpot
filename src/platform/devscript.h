#ifndef VAM_DEVSCRIPT_H
#define VAM_DEVSCRIPT_H

#include <SDL.h>
#include <stdbool.h>

#ifndef __vita__
bool devscript_init(void);
/* Feeds scripted input; returns true while a script is running. */
bool devscript_frame(bool *quit);
/* Call after drawing, before present. */
void devscript_after_render(SDL_Renderer *r);
#else
static inline bool devscript_init(void) { return false; }
static inline bool devscript_frame(bool *quit) { (void)quit; return false; }
static inline void devscript_after_render(SDL_Renderer *r) { (void)r; }
#endif

#endif
