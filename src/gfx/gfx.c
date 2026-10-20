#include "gfx.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VERTS 1024
#define MAX_INDICES (MAX_VERTS * 3)
#define CLIP_STACK 16

static SDL_Renderer *g_r;
static SDL_Vertex g_verts[MAX_VERTS];
static int g_indices[MAX_INDICES];
static SDL_FPoint g_path[MAX_VERTS];
static rectf g_clip[CLIP_STACK];
static int g_clip_n;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

rectf rect_intersect(rectf a, rectf b)
{
    float x0 = a.x > b.x ? a.x : b.x;
    float y0 = a.y > b.y ? a.y : b.y;
    float x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    rectf r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return r;
}

rectf rect_inset(rectf r, float d)
{
    rectf o = { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d };
    return o;
}

color color_lerp(color a, color b, float t)
{
    t = clampf(t, 0, 1);
    color c = {
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t),
        (uint8_t)(a.a + (b.a - a.a) * t),
    };
    return c;
}

color color_scale(color c, float k)
{
    color o = { (uint8_t)clampf(c.r * k, 0, 255), (uint8_t)clampf(c.g * k, 0, 255),
                (uint8_t)clampf(c.b * k, 0, 255), c.a };
    return o;
}

static SDL_Color sdlc(color c)
{
    SDL_Color s = { c.r, c.g, c.b, c.a };
    return s;
}

void gfx_init(SDL_Renderer *renderer)
{
    g_r = renderer;
    g_clip_n = 0;
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
}

SDL_Renderer *gfx_renderer(void) { return g_r; }

/* Textures waiting to be destroyed. Flushed in batches so the GPU wait in
 * the Vita's DestroyTexture happens once per batch, not once per texture. */
#define DEAD_MAX 256
#define DEAD_BATCH 24
#define DEAD_MAX_AGE_MS 1000
static SDL_Texture *g_dead[DEAD_MAX];
static int g_dead_n;
static Uint64 g_dead_since;

void gfx_destroy_texture_later(SDL_Texture *tex)
{
    if (!tex)
        return;
    if (g_dead_n == DEAD_MAX) {
        SDL_DestroyTexture(tex); /* backlog full: rare, pay the stall now */
        return;
    }
    if (g_dead_n == 0)
        g_dead_since = SDL_GetTicks64();
    g_dead[g_dead_n++] = tex;
}

void gfx_flush_destroyed(bool force)
{
    if (g_dead_n == 0)
        return;
    if (!force && g_dead_n < DEAD_BATCH && SDL_GetTicks64() - g_dead_since < DEAD_MAX_AGE_MS)
        return;
    for (int i = 0; i < g_dead_n; i++)
        SDL_DestroyTexture(g_dead[i]);
    g_dead_n = 0;
}

void gfx_clear(color c)
{
    SDL_SetRenderDrawColor(g_r, c.r, c.g, c.b, c.a);
    SDL_RenderClear(g_r);
}

static void apply_clip(void)
{
    if (g_clip_n == 0) {
        SDL_RenderSetClipRect(g_r, NULL);
        return;
    }
    rectf c = g_clip[g_clip_n - 1];
    SDL_Rect r = { (int)floorf(c.x), (int)floorf(c.y), (int)ceilf(c.w), (int)ceilf(c.h) };
    if (r.w <= 0 || r.h <= 0) {
        /* SDL treats an empty clip as "no clip"; use a 1px offscreen rect. */
        r.x = -10; r.y = -10; r.w = 1; r.h = 1;
    }
    SDL_RenderSetClipRect(g_r, &r);
}

void gfx_push_clip(rectf r)
{
    if (g_clip_n > 0)
        r = rect_intersect(r, g_clip[g_clip_n - 1]);
    if (g_clip_n < CLIP_STACK)
        g_clip[g_clip_n++] = r;
    apply_clip();
}

void gfx_pop_clip(void)
{
    if (g_clip_n > 0)
        g_clip_n--;
    apply_clip();
}

rectf gfx_clip(void)
{
    if (g_clip_n == 0)
        return rect(0, 0, 100000, 100000);
    return g_clip[g_clip_n - 1];
}

/* Cheap rejection of shapes fully outside the current clip. */
static bool culled(float x, float y, float w, float h)
{
    if (g_clip_n == 0)
        return false;
    rectf c = g_clip[g_clip_n - 1];
    return x > c.x + c.w || y > c.y + c.h || x + w < c.x || y + h < c.y;
}

void gfx_rect(rectf r, color c)
{
    if (c.a == 0 || culled(r.x, r.y, r.w, r.h))
        return;
    SDL_SetRenderDrawColor(g_r, c.r, c.g, c.b, c.a);
    SDL_FRect fr = { r.x, r.y, r.w, r.h };
    SDL_RenderFillRectF(g_r, &fr);
}

static int corner_segments(float radius)
{
    int seg = (int)(radius / 2.0f);
    return seg < 3 ? 3 : seg > 10 ? 10 : seg;
}

/* Unit-circle tables: shapes are rebuilt every frame and the Vita's
 * cosf/sinf are slow enough to show up. */
#define CIRCLE_MAX_SEG 64
#define CORNER_MAX_SEG 10
static float g_quarter[CORNER_MAX_SEG + 1][CORNER_MAX_SEG + 1][2];
static bool g_quarter_ok[CORNER_MAX_SEG + 1];
static float g_unit[CIRCLE_MAX_SEG + 1][CIRCLE_MAX_SEG][2];
static bool g_unit_ok[CIRCLE_MAX_SEG + 1];

/* cos/sin of (pi/2) * i / seg for i in [0, seg]. */
static const float (*quarter_table(int seg))[2]
{
    if (!g_quarter_ok[seg]) {
        for (int i = 0; i <= seg; i++) {
            float a = (float)(M_PI / 2) * i / seg;
            g_quarter[seg][i][0] = cosf(a);
            g_quarter[seg][i][1] = sinf(a);
        }
        g_quarter_ok[seg] = true;
    }
    return (const float (*)[2])g_quarter[seg];
}

/* cos/sin of 2pi * i / seg for i in [0, seg). */
static const float (*circle_table(int seg))[2]
{
    if (!g_unit_ok[seg]) {
        for (int i = 0; i < seg; i++) {
            float a = (float)(2 * M_PI) * i / seg;
            g_unit[seg][i][0] = cosf(a);
            g_unit[seg][i][1] = sinf(a);
        }
        g_unit_ok[seg] = true;
    }
    return (const float (*)[2])g_unit[seg];
}

/* Circle of seg points into g_path. */
static void circle_path(float cx, float cy, float radius, int seg)
{
    const float (*t)[2] = circle_table(seg);
    for (int i = 0; i < seg; i++) {
        g_path[i].x = cx + t[i][0] * radius;
        g_path[i].y = cy + t[i][1] * radius;
    }
}

/* Outline of a rounded rect, clockwise, into g_path. */
static int rrect_path(rectf r, float rad)
{
    rad = clampf(rad, 0, (r.w < r.h ? r.w : r.h) * 0.5f);
    int seg = corner_segments(rad);
    const float (*t)[2] = quarter_table(seg);
    float cx[4] = { r.x + r.w - rad, r.x + r.w - rad, r.x + rad, r.x + rad };
    float cy[4] = { r.y + rad, r.y + r.h - rad, r.y + r.h - rad, r.y + rad };
    /* Corner k starts at angle (k - 1) * pi/2: rotate (cos, sin) to match. */
    int n = 0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= seg; i++) {
            float c = t[i][0], s = t[i][1], x, y;
            switch (k) {
            case 0: x = s; y = -c; break;
            case 1: x = c; y = s; break;
            case 2: x = -s; y = c; break;
            default: x = -c; y = -s; break;
            }
            g_path[n].x = cx[k] + x * rad;
            g_path[n].y = cy[k] + y * rad;
            n++;
        }
    }
    return n;
}

/* Fill the convex polygon in g_path[0..n) as a fan around (cx, cy). */
static void fill_fan(int n, float cx, float cy, color c, SDL_Texture *tex, rectf uv)
{
    if (n < 3 || n + 1 > MAX_VERTS)
        return;
    SDL_Color sc = sdlc(c);
    g_verts[0].position.x = cx;
    g_verts[0].position.y = cy;
    g_verts[0].color = sc;
    g_verts[0].tex_coord.x = uv.w > 0 ? (cx - uv.x) / uv.w : 0;
    g_verts[0].tex_coord.y = uv.h > 0 ? (cy - uv.y) / uv.h : 0;
    for (int i = 0; i < n; i++) {
        SDL_Vertex *v = &g_verts[i + 1];
        v->position = g_path[i];
        v->color = sc;
        v->tex_coord.x = uv.w > 0 ? (g_path[i].x - uv.x) / uv.w : 0;
        v->tex_coord.y = uv.h > 0 ? (g_path[i].y - uv.y) / uv.h : 0;
    }
    int ni = 0;
    for (int i = 0; i < n; i++) {
        g_indices[ni++] = 0;
        g_indices[ni++] = i + 1;
        g_indices[ni++] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(g_r, tex, g_verts, n + 1, g_indices, ni);
}

void gfx_rrect(rectf r, float radius, color c)
{
    if (c.a == 0 || r.w <= 0 || r.h <= 0 || culled(r.x, r.y, r.w, r.h))
        return;
    if (radius < 0.5f) {
        gfx_rect(r, c);
        return;
    }
    int n = rrect_path(r, radius);
    fill_fan(n, r.x + r.w / 2, r.y + r.h / 2, c, NULL, rect(0, 0, 0, 0));
}

/* Thick closed polyline over g_path as a triangle strip. */
static void stroke_closed(int n, float thick, color c)
{
    if (n < 2 || n * 2 > MAX_VERTS)
        return;
    SDL_Color sc = sdlc(c);
    float h = thick * 0.5f;
    for (int i = 0; i < n; i++) {
        SDL_FPoint p = g_path[i];
        SDL_FPoint a = g_path[(i + n - 1) % n];
        SDL_FPoint b = g_path[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.0001f) len = 1;
        float nx = -dy / len * h, ny = dx / len * h;
        g_verts[i * 2].position.x = p.x + nx;
        g_verts[i * 2].position.y = p.y + ny;
        g_verts[i * 2 + 1].position.x = p.x - nx;
        g_verts[i * 2 + 1].position.y = p.y - ny;
        g_verts[i * 2].color = g_verts[i * 2 + 1].color = sc;
    }
    int ni = 0;
    for (int i = 0; i < n; i++) {
        int a0 = i * 2, a1 = i * 2 + 1, b0 = ((i + 1) % n) * 2, b1 = b0 + 1;
        g_indices[ni++] = a0; g_indices[ni++] = a1; g_indices[ni++] = b0;
        g_indices[ni++] = b0; g_indices[ni++] = a1; g_indices[ni++] = b1;
    }
    SDL_RenderGeometry(g_r, NULL, g_verts, n * 2, g_indices, ni);
}

void gfx_rrect_outline(rectf r, float radius, float thick, color c)
{
    if (c.a == 0 || culled(r.x - thick, r.y - thick, r.w + 2 * thick, r.h + 2 * thick))
        return;
    int n = rrect_path(r, radius);
    stroke_closed(n, thick, c);
}

void gfx_circle(float cx, float cy, float radius, color c)
{
    if (c.a == 0 || radius <= 0 || culled(cx - radius, cy - radius, radius * 2, radius * 2))
        return;
    int seg = (int)(radius * 1.2f);
    seg = seg < 12 ? 12 : seg > CIRCLE_MAX_SEG ? CIRCLE_MAX_SEG : seg;
    circle_path(cx, cy, radius, seg);
    fill_fan(seg, cx, cy, c, NULL, rect(0, 0, 0, 0));
}

void gfx_arc(float cx, float cy, float radius, float thick, float a0, float a1, color c)
{
    float ro = radius + thick * 0.5f, ri = radius - thick * 0.5f;
    if (ri < 0) ri = 0;
    if (c.a == 0 || culled(cx - ro, cy - ro, ro * 2, ro * 2))
        return;
    int seg = (int)(fabsf(a1 - a0) * ro * 0.6f);
    seg = seg < 6 ? 6 : seg > 96 ? 96 : seg;
    SDL_Color sc = sdlc(c);
    for (int i = 0; i <= seg; i++) {
        float a = a0 + (a1 - a0) * i / seg;
        float ca = cosf(a), sa = sinf(a);
        g_verts[i * 2].position.x = cx + ca * ro;
        g_verts[i * 2].position.y = cy + sa * ro;
        g_verts[i * 2 + 1].position.x = cx + ca * ri;
        g_verts[i * 2 + 1].position.y = cy + sa * ri;
        g_verts[i * 2].color = g_verts[i * 2 + 1].color = sc;
    }
    int ni = 0;
    for (int i = 0; i < seg; i++) {
        int a = i * 2, b = i * 2 + 2;
        g_indices[ni++] = a; g_indices[ni++] = a + 1; g_indices[ni++] = b;
        g_indices[ni++] = b; g_indices[ni++] = a + 1; g_indices[ni++] = b + 1;
    }
    SDL_RenderGeometry(g_r, NULL, g_verts, (seg + 1) * 2, g_indices, ni);
}

void gfx_line(float x0, float y0, float x1, float y1, float thick, color c)
{
    if (c.a == 0)
        return;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f)
        return;
    float h = thick * 0.5f;
    float nx = -dy / len * h, ny = dx / len * h;
    SDL_Color sc = sdlc(c);
    SDL_Vertex v[4] = {
        { { x0 + nx, y0 + ny }, sc, { 0, 0 } },
        { { x0 - nx, y0 - ny }, sc, { 0, 0 } },
        { { x1 + nx, y1 + ny }, sc, { 0, 0 } },
        { { x1 - nx, y1 - ny }, sc, { 0, 0 } },
    };
    int idx[6] = { 0, 1, 2, 2, 1, 3 };
    SDL_RenderGeometry(g_r, NULL, v, 4, idx, 6);
    /* Round caps keep joints of multi-segment icons tidy. */
    if (thick >= 2.0f) {
        gfx_circle(x0, y0, h, c);
        gfx_circle(x1, y1, h, c);
    }
}

void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, color c)
{
    if (c.a == 0)
        return;
    SDL_Color sc = sdlc(c);
    SDL_Vertex v[3] = {
        { { x0, y0 }, sc, { 0, 0 } },
        { { x1, y1 }, sc, { 0, 0 } },
        { { x2, y2 }, sc, { 0, 0 } },
    };
    SDL_RenderGeometry(g_r, NULL, v, 3, NULL, 0);
}

void gfx_vgradient(rectf r, color top, color bottom)
{
    if (culled(r.x, r.y, r.w, r.h))
        return;
    SDL_Color a = sdlc(top), b = sdlc(bottom);
    SDL_Vertex v[4] = {
        { { r.x, r.y }, a, { 0, 0 } },
        { { r.x + r.w, r.y }, a, { 0, 0 } },
        { { r.x, r.y + r.h }, b, { 0, 0 } },
        { { r.x + r.w, r.y + r.h }, b, { 0, 0 } },
    };
    int idx[6] = { 0, 1, 2, 2, 1, 3 };
    SDL_RenderGeometry(g_r, NULL, v, 4, idx, 6);
}

void gfx_hgradient(rectf r, color left, color right)
{
    if (culled(r.x, r.y, r.w, r.h))
        return;
    SDL_Color a = sdlc(left), b = sdlc(right);
    SDL_Vertex v[4] = {
        { { r.x, r.y }, a, { 0, 0 } },
        { { r.x + r.w, r.y }, b, { 0, 0 } },
        { { r.x, r.y + r.h }, a, { 0, 0 } },
        { { r.x + r.w, r.y + r.h }, b, { 0, 0 } },
    };
    int idx[6] = { 0, 1, 2, 2, 1, 3 };
    SDL_RenderGeometry(g_r, NULL, v, 4, idx, 6);
}

void gfx_shadow(rectf r, float radius, float spread, uint8_t alpha)
{
    const int steps = 4;
    for (int i = steps; i >= 1; i--) {
        float d = spread * i / steps;
        rectf s = { r.x - d, r.y - d + spread * 0.35f, r.w + 2 * d, r.h + 2 * d };
        gfx_rrect(s, radius + d, rgba(0x000000, (uint8_t)(alpha / (steps + 1))));
    }
}

void gfx_image(SDL_Texture *tex, rectf r, float radius, uint8_t alpha)
{
    if (!tex || alpha == 0 || culled(r.x, r.y, r.w, r.h))
        return;
    SDL_SetTextureAlphaMod(tex, alpha);
    float half = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < half - 0.01f) {
        /* A textured rounded-rect fan made cover art blink out for single
           frames on the Vita (Home only, every scroll); a plain quad doesn't. */
        SDL_FRect d = { r.x, r.y, r.w, r.h };
        SDL_RenderCopyF(g_r, tex, NULL, &d);
        SDL_SetTextureAlphaMod(tex, 255);
        return;
    }
    circle_path(r.x + r.w / 2, r.y + r.h / 2, half, 48);
    fill_fan(48, r.x + r.w / 2, r.y + r.h / 2, rgb(0xFFFFFF), tex, r);
    SDL_SetTextureAlphaMod(tex, 255);
}

/* ------------------------------------------------------------------------ */
/* Icons, designed on a 24x24 grid.                                          */
/* ------------------------------------------------------------------------ */

typedef struct grid { float cx, cy, u; } grid;
static float GX(grid g, float x) { return g.cx + (x - 12) * g.u; }
static float GY(grid g, float y) { return g.cy + (y - 12) * g.u; }
static void gl(grid g, float x0, float y0, float x1, float y1, float t, color c)
{
    gfx_line(GX(g, x0), GY(g, y0), GX(g, x1), GY(g, y1), t * g.u, c);
}
static void gt(grid g, float x0, float y0, float x1, float y1, float x2, float y2, color c)
{
    gfx_triangle(GX(g, x0), GY(g, y0), GX(g, x1), GY(g, y1), GX(g, x2), GY(g, y2), c);
}
static void gr(grid g, float x, float y, float w, float h, color c)
{
    gfx_rect(rect(GX(g, x), GY(g, y), w * g.u, h * g.u), c);
}

static int heart_path(grid g)
{
    enum { n = 56 };
    static float hx[n], hy[n];
    static bool ready;
    if (!ready) {
        for (int i = 0; i < n; i++) {
            float t = (float)(2 * M_PI) * i / n;
            float s = sinf(t);
            hx[i] = 12 + 16 * s * s * s * 0.56f;
            hy[i] = 11.2f - (13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t)) * 0.56f;
        }
        ready = true;
    }
    for (int i = 0; i < n; i++) {
        g_path[i].x = GX(g, hx[i]);
        g_path[i].y = GY(g, hy[i]);
    }
    return n;
}

void gfx_icon(icon_id id, float cx, float cy, float size, color c)
{
    grid g = { cx, cy, size / 24.0f };
    switch (id) {
    case ICON_HOME:
        gl(g, 3, 11, 12, 3.5f, 2, c);
        gl(g, 12, 3.5f, 21, 11, 2, c);
        gl(g, 5.5f, 10, 5.5f, 20.5f, 2, c);
        gl(g, 18.5f, 10, 18.5f, 20.5f, 2, c);
        gl(g, 5.5f, 20.5f, 18.5f, 20.5f, 2, c);
        break;
    case ICON_HOME_FILLED:
        gt(g, 2.5f, 11.5f, 12, 2.8f, 21.5f, 11.5f, c);
        gr(g, 4.8f, 10, 14.4f, 11.5f, c);
        break;
    case ICON_SEARCH:
    case ICON_SEARCH_BOLD: {
        float t = id == ICON_SEARCH_BOLD ? 2.8f : 2.0f;
        gfx_arc(GX(g, 10.5f), GY(g, 10.5f), 6.5f * g.u, t * g.u, 0, (float)(2 * M_PI), c);
        gl(g, 15.5f, 15.5f, 20.5f, 20.5f, t + 0.2f, c);
        break;
    }
    case ICON_LIBRARY:
    case ICON_LIBRARY_BOLD: {
        float t = id == ICON_LIBRARY_BOLD ? 2.8f : 2.0f;
        gl(g, 5, 4, 5, 20, t, c);
        gl(g, 10, 4, 10, 20, t, c);
        gl(g, 14.5f, 5, 19.5f, 20, t, c);
        break;
    }
    case ICON_PLAY:
        gt(g, 7.5f, 4.5f, 7.5f, 19.5f, 19.5f, 12, c);
        break;
    case ICON_PAUSE:
        gr(g, 6, 5, 4, 14, c);
        gr(g, 14, 5, 4, 14, c);
        break;
    case ICON_NEXT:
        gt(g, 5.5f, 5, 5.5f, 19, 16, 12, c);
        gr(g, 16, 5, 2.6f, 14, c);
        break;
    case ICON_PREV:
        gt(g, 18.5f, 5, 18.5f, 19, 8, 12, c);
        gr(g, 5.4f, 5, 2.6f, 14, c);
        break;
    case ICON_SHUFFLE:
        gl(g, 3, 7, 8, 7, 1.8f, c);
        gl(g, 8, 7, 16, 17, 1.8f, c);
        gl(g, 16, 17, 20.5f, 17, 1.8f, c);
        gl(g, 3, 17, 8, 17, 1.8f, c);
        gl(g, 8, 17, 16, 7, 1.8f, c);
        gl(g, 16, 7, 20.5f, 7, 1.8f, c);
        gt(g, 18.5f, 3.8f, 22, 7, 18.5f, 10.2f, c);
        gt(g, 18.5f, 13.8f, 22, 17, 18.5f, 20.2f, c);
        break;
    case ICON_REPEAT:
        gfx_rrect_outline(rect(GX(g, 3.5f), GY(g, 6.5f), 17 * g.u, 11 * g.u), 4 * g.u, 1.8f * g.u, c);
        gt(g, 11, 3.5f, 15, 6.5f, 11, 9.5f, c);
        break;
    case ICON_HEART:
        fill_fan(heart_path(g), GX(g, 12), GY(g, 11), c, NULL, rect(0, 0, 0, 0));
        break;
    case ICON_HEART_OUTLINE:
        stroke_closed(heart_path(g), 1.8f * g.u, c);
        break;
    case ICON_BACK:
        gl(g, 15, 4.5f, 8, 12, 2.4f, c);
        gl(g, 8, 12, 15, 19.5f, 2.4f, c);
        break;
    case ICON_CHEVRON_DOWN:
        gl(g, 5, 9, 12, 16, 2.4f, c);
        gl(g, 12, 16, 19, 9, 2.4f, c);
        break;
    case ICON_MORE:
        gfx_circle(GX(g, 5), GY(g, 12), 1.9f * g.u, c);
        gfx_circle(GX(g, 12), GY(g, 12), 1.9f * g.u, c);
        gfx_circle(GX(g, 19), GY(g, 12), 1.9f * g.u, c);
        break;
    case ICON_QUEUE:
        gl(g, 3, 5.5f, 16, 5.5f, 1.8f, c);
        gl(g, 3, 10.5f, 16, 10.5f, 1.8f, c);
        gl(g, 3, 15.5f, 10, 15.5f, 1.8f, c);
        gt(g, 13.5f, 12.5f, 13.5f, 21, 20.5f, 16.75f, c);
        break;
    case ICON_NOTE:
        gfx_circle(GX(g, 8), GY(g, 17), 3.2f * g.u, c);
        gl(g, 11, 17, 11, 4.5f, 1.8f, c);
        gl(g, 11, 4.5f, 18, 6.5f, 1.8f, c);
        gl(g, 11, 7.5f, 18, 9.5f, 1.8f, c);
        break;
    case ICON_PERSON:
        gfx_circle(GX(g, 12), GY(g, 8), 4.2f * g.u, c);
        gfx_arc(GX(g, 12), GY(g, 21.5f), 4 * g.u, 8 * g.u, (float)M_PI, (float)(2 * M_PI), c);
        break;
    case ICON_DISC:
        gfx_arc(GX(g, 12), GY(g, 12), 8 * g.u, 2 * g.u, 0, (float)(2 * M_PI), c);
        gfx_circle(GX(g, 12), GY(g, 12), 2.2f * g.u, c);
        break;
    case ICON_PLUS:
        gl(g, 12, 4.5f, 12, 19.5f, 2, c);
        gl(g, 4.5f, 12, 19.5f, 12, 2, c);
        break;
    case ICON_CHECK:
        gl(g, 5, 12.5f, 10, 17.5f, 2.4f, c);
        gl(g, 10, 17.5f, 19.5f, 7, 2.4f, c);
        break;
    case ICON_CLOSE:
        gl(g, 6, 6, 18, 18, 2.2f, c);
        gl(g, 18, 6, 6, 18, 2.2f, c);
        break;
    case ICON_VOLUME:
        gr(g, 3, 9, 4, 6, c);
        gt(g, 6, 9, 12.5f, 4, 12.5f, 20, c);
        gt(g, 6, 9, 12.5f, 20, 6, 15, c);
        gfx_arc(GX(g, 13), GY(g, 12), 4.5f * g.u, 1.8f * g.u, -0.9f, 0.9f, c);
        gfx_arc(GX(g, 13), GY(g, 12), 8 * g.u, 1.8f * g.u, -0.9f, 0.9f, c);
        break;
    case ICON_DEVICE:
        gfx_rrect_outline(rect(GX(g, 2.5f), GY(g, 7), 19 * g.u, 10 * g.u), 3 * g.u, 1.8f * g.u, c);
        gfx_circle(GX(g, 7), GY(g, 12), 1.6f * g.u, c);
        gfx_circle(GX(g, 17), GY(g, 12), 1.6f * g.u, c);
        break;
    case ICON_SETTINGS:
    case ICON_SETTINGS_BOLD: {
        /* Gear: a ring with eight teeth. */
        float t = id == ICON_SETTINGS_BOLD ? 2.8f : 2.0f;
        gfx_arc(GX(g, 12), GY(g, 12), 6 * g.u, t * g.u, 0, (float)(2 * M_PI), c);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4, ca = cosf(a), sa = sinf(a);
            gl(g, 12 + ca * 6.5f, 12 + sa * 6.5f, 12 + ca * 10, 12 + sa * 10, 3.2f, c);
        }
        break;
    }
    }
}

void gfx_equalizer(float cx, float cy, float size, float time, bool animate, color c)
{
    float w = size / 5.0f;
    float base = cy + size / 2;
    for (int i = 0; i < 3; i++) {
        float h = animate ? (0.35f + 0.65f * fabsf(sinf(time * (3.1f + i * 1.7f) + i)))
                          : (i == 1 ? 1.0f : 0.55f);
        float bh = size * h;
        gfx_rect(rect(cx - size / 2 + i * w * 2, base - bh, w, bh), c);
    }
}

void gfx_spinner(float cx, float cy, float radius, float time, color c)
{
    float a0 = time * 5.0f;
    float len = 1.6f + 0.9f * sinf(time * 2.3f);
    gfx_arc(cx, cy, radius, radius * 0.22f, a0, a0 + len, c);
}
