#ifndef VAM_UI_H
#define VAM_UI_H

/*
 * Immediate-mode input layer: pointer (mouse / front touch), D-pad focus
 * navigation, scrolling with momentum. Widgets call ui_item() every frame
 * with a stable id and their on-screen rect.
 */

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "../gfx/gfx.h"
#include "../platform/platform.h"

typedef enum axis { AXIS_X = 1, AXIS_Y = 2 } axis;

typedef struct scroller {
    float pos;
    float vel;
    float target;      /* animate toward when has_target */
    bool has_target;
    float max;         /* updated by ui_scroll_begin */
} scroller;

typedef struct ui_res {
    bool clicked;      /* tap released inside, or Confirm while focused  */
    bool menu;         /* long-press, or Square while focused            */
    bool pressed;      /* pointer held inside (draw pressed state)       */
    bool focused;      /* has D-pad focus and focus is visible           */
    int slide;         /* slider items: -1/+1 from Left/Right            */
} ui_res;

enum {
    UI_F_SLIDER = 1 << 0,   /* Left/Right adjust instead of moving focus */
    UI_F_NOFOCUS = 1 << 1,  /* pointer only                              */
};

void ui_init(void);
void ui_handle_event(const SDL_Event *e);
void ui_begin_frame(float dt);
void ui_end_frame(void);

/* Text entry mode: keyboard goes to the caller, not to navigation. */
void ui_set_text_mode(bool on);
bool ui_text_mode(void);

/* Disable item interaction (used to freeze screens under an overlay). */
void ui_set_enabled(bool on);
bool ui_enabled(void);

/* Buttons pressed this frame (with D-pad auto-repeat). */
bool ui_btn(btn b);
void ui_consume(btn b);

/* A two-button combo (BTN_COUNT = none). A press of either button waits
 * briefly for the other before acting on its own, so the combo doesn't also
 * trigger them. ui_combo_fired() is true for the frame the combo lands. */
void ui_set_combo(btn a, btn b);
bool ui_combo_fired(void);
/* Rebinding the combo: while capturing, buttons do nothing, and the first two
 * held together are handed out once by ui_captured_combo(). */
void ui_capture_combo(bool on);
bool ui_capturing_combo(void);
bool ui_captured_combo(btn *a, btn *b);

/* Pointer clip for hit testing (matches drawing clip). */
void ui_push_clip(rectf r);
void ui_pop_clip(void);

uint32_t ui_id(const char *s);
uint32_t ui_idx(uint32_t base, uint32_t n);

ui_res ui_item(uint32_t id, rectf r, unsigned flags);

/* Focus */
uint32_t ui_focus(void);
void ui_set_focus(uint32_t id);
bool ui_focus_visible(void);
/* Forget last frame's focusables (call after navigation changes screens). */
void ui_reset_focus_list(void);
bool ui_has_focusables(void);

/* Scroll areas. Items registered between begin/end belong to the area,
 * so D-pad focus scrolls them into view. */
void ui_scroll_begin(scroller *s, rectf viewport, float content_len, axis ax, bool stick);
void ui_scroll_end(void);
void ui_scroll_to(scroller *s, float pos);

/* Pointer state for custom widgets (sliders). */
bool ui_pointer_down(void);
float ui_pointer_x(void);
float ui_pointer_y(void);
bool ui_pointer_dragging(void);
bool ui_pointer_pressed_in(rectf r);
uint64_t ui_ticks(void);

#endif
