#ifndef VAM_GFX_H
#define VAM_GFX_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct color { uint8_t r, g, b, a; } color;
typedef struct rectf { float x, y, w, h; } rectf;

static inline color rgb(uint32_t hex)
{
    color c = { (uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, 255 };
    return c;
}

static inline color rgba(uint32_t hex, uint8_t a)
{
    color c = rgb(hex);
    c.a = a;
    return c;
}

static inline color with_alpha(color c, float a)
{
    float v = c.a * a;
    c.a = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    return c;
}

static inline rectf rect(float x, float y, float w, float h)
{
    rectf r = { x, y, w, h };
    return r;
}

static inline bool rect_contains(rectf r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

rectf rect_intersect(rectf a, rectf b);
rectf rect_inset(rectf r, float d);
color color_lerp(color a, color b, float t);
color color_scale(color c, float k);

typedef enum icon_id {
    ICON_HOME,
    ICON_HOME_FILLED,
    ICON_SEARCH,
    ICON_SEARCH_BOLD,
    ICON_LIBRARY,
    ICON_LIBRARY_BOLD,
    ICON_PLAY,
    ICON_PAUSE,
    ICON_NEXT,
    ICON_PREV,
    ICON_SHUFFLE,
    ICON_REPEAT,
    ICON_HEART,
    ICON_HEART_OUTLINE,
    ICON_BACK,
    ICON_CHEVRON_DOWN,
    ICON_MORE,
    ICON_QUEUE,
    ICON_NOTE,
    ICON_PERSON,
    ICON_DISC,
    ICON_PLUS,
    ICON_CHECK,
    ICON_CLOSE,
    ICON_VOLUME,
    ICON_DEVICE
} icon_id;

void gfx_init(SDL_Renderer *renderer);
SDL_Renderer *gfx_renderer(void);

/* Destroy a texture between frames instead of now. On the Vita every
 * SDL_DestroyTexture waits for the GPU (sceGxmFinish), and destroying a
 * texture mid-frame can flush the batched draws early. */
void gfx_destroy_texture_later(SDL_Texture *tex);
/* Call right after SDL_RenderPresent; force also empties a small backlog. */
void gfx_flush_destroyed(bool force);

void gfx_clear(color c);
void gfx_push_clip(rectf r);
void gfx_pop_clip(void);
rectf gfx_clip(void);

void gfx_rect(rectf r, color c);
void gfx_rrect(rectf r, float radius, color c);
void gfx_rrect_outline(rectf r, float radius, float thick, color c);
void gfx_circle(float cx, float cy, float radius, color c);
/* Ring segment, angles in radians, 0 = +x, clockwise (screen y down). */
void gfx_arc(float cx, float cy, float radius, float thick, float a0, float a1, color c);
void gfx_line(float x0, float y0, float x1, float y1, float thick, color c);
void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, color c);
void gfx_vgradient(rectf r, color top, color bottom);
void gfx_hgradient(rectf r, color left, color right);
/* Soft drop shadow behind r. */
void gfx_shadow(rectf r, float radius, float spread, uint8_t alpha);
/* Draw a texture into r with rounded corners (radius >= half size = circle). */
void gfx_image(SDL_Texture *tex, rectf r, float radius, uint8_t alpha);
/* Icon drawn in a size x size box centred on (cx, cy). */
void gfx_icon(icon_id id, float cx, float cy, float size, color c);
/* Animated "now playing" bars. */
void gfx_equalizer(float cx, float cy, float size, float time, bool animate, color c);
void gfx_spinner(float cx, float cy, float radius, float time, color c);

#endif
