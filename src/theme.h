#ifndef VAM_THEME_H
#define VAM_THEME_H

/* Screen geometry (PS Vita native resolution). */
#define SCREEN_W 960
#define SCREEN_H 544

#define SIDEBAR_W 76
#define PLAYER_H 84
#define CONTENT_X SIDEBAR_W
#define CONTENT_Y 0
#define CONTENT_W (SCREEN_W - SIDEBAR_W)
#define CONTENT_H (SCREEN_H - PLAYER_H)
#define PAD 20

/* Smallest touch target. The Vita's 5" 220 ppi panel makes 1 px ~0.115 mm,
   so 48 px is about 5.5 mm. */
#define TOUCH_MIN 48
#define SLIDER_HIT_H 44

/* Palette. Dark UI in the style of the official app, own accent tone. */
#define C_BG          0x121212
#define C_BG_DEEP     0x000000
#define C_SURFACE     0x181818
#define C_SURFACE_HI  0x282828
#define C_SURFACE_HI2 0x333333
#define C_TEXT        0xFFFFFF
#define C_SUBTEXT     0xB3B3B3
#define C_MUTED       0x727272
#define C_DIVIDER     0x2A2A2A
#define C_ACCENT      0x1ED760
#define C_ACCENT_DIM  0x169C46
#define C_ERROR       0xF15E6C
#define C_LIKED_TOP   0x4B2BD8
#define C_LIKED_BOT   0x8FB9C9

/* Font sizes in px. text.c rounds to the nearest preloaded size,
   so keep its SIZES table in step. */
#define FS_XS   14
#define FS_S    16
#define FS_M    19
#define FS_L    22
#define FS_XL   24
#define FS_XXL  32
#define FS_HUGE 44

/* Motion */
#define ANIM_FAST 0.12f
#define ANIM_MED  0.22f

#endif
