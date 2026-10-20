#ifndef VAM_PREFS_H
#define VAM_PREFS_H

/* User settings, saved as key=value lines in <data dir>/settings.txt. */

#include "platform/platform.h"

typedef struct prefs {
    btn combo[2];   /* screen-off combo                    */
    int dim_after;  /* seconds without input, 0 = never     */
    int off_after;  /* seconds without input, 0 = never     */
    int bitrate;    /* streaming quality: 96, 160 or 320    */
    bool lyrics;    /* Now Playing shows lyrics, not the art */
} prefs;

/* Defaults, then whatever settings.txt has. Call before vam_init. */
void prefs_load(void);
prefs *prefs_get(void);
/* Call after changing prefs: the file is written once the changes settle, so
 * cycling through options doesn't hit the memory card on every press. */
void prefs_changed(void);
/* Once per frame: writes settings.txt when a change has settled. */
void prefs_frame(void);
/* Writes pending changes now (at shutdown). */
void prefs_flush(void);

#endif
