#include <stdio.h>
#include <string.h>

#include "../app.h"
#include "../gfx/text.h"
#include "../theme.h"

#define ROW_H 56
#define ENTITY_ROW_H 64
#define CARD_W 152
#define CARD_GAP 12
#define CARD_IMG 132
#define CARD_H 202
#define GRID_H 56
#define GRID_GAP 10
#define CULL_MARGIN 280

static bool is_playable_kind(int k) { return k == VAM_KIND_TRACK || k == VAM_KIND_EPISODE; }

static void activate(sections_ctx *c, const vam_item *it, uint32_t index)
{
    if (is_playable_kind(it->kind))
        app_play_item(it, c->context_uri, index);
    else
        app_open_uri(it->uri);
}

static bool near_view(sections_ctx *c, float y, float h)
{
    return y + h >= c->viewport.y - CULL_MARGIN && y <= c->viewport.y + c->viewport.h + CULL_MARGIN;
}

static bool in_view(sections_ctx *c, float y, float h)
{
    return y + h >= c->viewport.y && y <= c->viewport.y + c->viewport.h;
}

static void explicit_badge(float x, float cy)
{
    gfx_rrect(rect(x, cy - 7, 14, 14), 2, rgb(C_SUBTEXT));
    text_draw_aligned("E", x + 7, cy - text_line_height(FS_XS) / 2.0f, FS_XS, FW_BOLD, rgb(0x121212), 0,
                      ALIGN_CENTER);
}

float draw_track_row(sections_ctx *c, const vam_item *it, uint32_t index, float y, uint32_t id_base)
{
    if (!near_view(c, y, ROW_H))
        return y + ROW_H;
    rectf r = rect(c->x, y, c->w, ROW_H);
    uint32_t id = ui_idx(id_base, index);
    ui_res res = ui_item(id, r, 0);
    if (!in_view(c, y, ROW_H)) {
        if (res.clicked) activate(c, it, index);
        return y + ROW_H;
    }

    bool current = app_is_current_track(it->uri);
    bool playable = it->is_playable || !is_playable_kind(it->kind);
    if (res.pressed || res.focused)
        gfx_rrect(r, 6, rgb(res.pressed ? C_SURFACE_HI2 : C_SURFACE_HI));

    float x = c->x + 12;
    float cy = y + ROW_H / 2;
    if (c->numbered) {
        if (current) {
            gfx_equalizer(x + 10, cy, 14, A.time, app_is_playing(), rgb(C_ACCENT));
        } else {
            char num[16];
            snprintf(num, sizeof(num), "%u", index + 1);
            text_draw_aligned(num, x + 10, cy - text_line_height(FS_M) / 2.0f, FS_M, FW_REGULAR,
                              rgb(C_SUBTEXT), 0, ALIGN_CENTER);
        }
        x += 34;
    } else if (c->show_art) {
        draw_cover(it->image_url, rect(x, cy - 20, 40, 40), 4, it->kind, it->title);
        x += 52;
    }

    /* Right side: heart (if saved), duration, more. */
    float right = c->x + c->w - 8;
    float more_cx = right - 16;
    bool more = icon_button(ui_idx(id, 7), ICON_MORE, more_cx, cy, 18, 32, rgb(C_SUBTEXT));
    float dur_r = more_cx - 26;
    if (it->duration_ms) {
        char d[16];
        fmt_duration(it->duration_ms, d, sizeof(d));
        text_draw_aligned(d, dur_r, cy - text_line_height(FS_S) / 2.0f, FS_S, FW_REGULAR, rgb(C_SUBTEXT), 0,
                          ALIGN_RIGHT);
    }
    float text_r = dur_r - 56;
    if (it->is_saved) {
        gfx_icon(ICON_HEART, dur_r - 70, cy, 16, rgb(C_ACCENT));
        text_r -= 28;
    }

    float tw = text_r - x;
    color title_c = current ? rgb(C_ACCENT) : playable ? rgb(C_TEXT) : rgb(C_MUTED);
    text_draw(it->title, x, y + 8, FS_M, FW_REGULAR, title_c, tw);
    float sx = x;
    if (it->is_explicit) {
        explicit_badge(sx, y + 38);
        sx += 20;
    }
    text_draw(it->subtitle, sx, y + 30, FS_S, FW_REGULAR, rgb(playable ? C_SUBTEXT : C_MUTED), text_r - sx);

    if (res.focused)
        draw_focus_ring(r, 6);
    if (res.clicked)
        activate(c, it, index);
    if (res.menu || more)
        app_open_sheet(it, c->context_uri, index);
    return y + ROW_H;
}

float draw_entity_row(sections_ctx *c, const vam_item *it, uint32_t index, float y, uint32_t id_base)
{
    if (!near_view(c, y, ENTITY_ROW_H))
        return y + ENTITY_ROW_H;
    rectf r = rect(c->x, y, c->w, ENTITY_ROW_H);
    ui_res res = ui_item(ui_idx(id_base, index), r, 0);
    if (in_view(c, y, ENTITY_ROW_H)) {
        if (res.pressed || res.focused)
            gfx_rrect(r, 6, rgb(res.pressed ? C_SURFACE_HI2 : C_SURFACE_HI));
        float s = 48;
        float rad = it->kind == VAM_KIND_ARTIST ? s / 2 : 4;
        draw_cover(it->image_url, rect(c->x + 8, y + 8, s, s), rad, it->kind, it->title);
        float tx = c->x + 8 + s + 14;
        bool cur = app_is_current_context(it->uri);
        float tw = c->w - (tx - c->x) - (cur ? 44 : 12);
        text_draw(it->title, tx, y + 12, FS_M, FW_REGULAR, cur ? rgb(C_ACCENT) : rgb(C_TEXT), tw);
        text_draw(it->subtitle, tx, y + 34, FS_S, FW_REGULAR, rgb(C_SUBTEXT), tw);
        if (cur)
            gfx_icon(ICON_VOLUME, c->x + c->w - 24, y + ENTITY_ROW_H / 2, 18, rgb(C_ACCENT));
        if (res.focused)
            draw_focus_ring(r, 6);
    }
    if (res.clicked)
        activate(c, it, index);
    if (res.menu)
        app_open_sheet(it, c->context_uri, index);
    return y + ENTITY_ROW_H;
}

static float draw_list(sections_ctx *c, model_section *s, uint32_t si, float y)
{
    uint32_t base = ui_idx(ui_id("list"), si);
    for (uint32_t i = 0; i < s->count; i++) {
        const vam_item *it = &s->items[i];
        if (is_playable_kind(it->kind))
            y = draw_track_row(c, it, i, y, base);
        else
            y = draw_entity_row(c, it, i, y, base);
    }
    /* Page in more rows as the end approaches (first section only). */
    if (si == 0 && model_has_more(c->v->page)) {
        float bottom = c->viewport.y + c->viewport.h;
        if (y < bottom + 900)
            app_request_more(c->v);
        if (c->v->more_req) {
            gfx_spinner(c->x + c->w / 2, y + 24, 12, A.time, rgb(C_SUBTEXT));
            y += 48;
        }
    }
    return y;
}

static float draw_grid(sections_ctx *c, model_section *s, uint32_t si, float y)
{
    const int cols = 3;
    float w = (c->w - GRID_GAP * (cols - 1)) / cols;
    uint32_t base = ui_idx(ui_id("grid"), si);
    for (uint32_t i = 0; i < s->count; i++) {
        float x = c->x + (i % cols) * (w + GRID_GAP);
        float ty = y + (i / cols) * (GRID_H + GRID_GAP);
        const vam_item *it = &s->items[i];
        rectf r = rect(x, ty, w, GRID_H);
        ui_res res = ui_item(ui_idx(base, i), r, 0);
        color bg = rgba(0xFFFFFF, res.pressed ? 46 : res.focused ? 40 : 22);
        gfx_rrect(r, 4, bg);
        float rad = it->kind == VAM_KIND_ARTIST ? GRID_H / 2 : 4;
        draw_cover(it->image_url, rect(x, ty, GRID_H, GRID_H), rad, it->kind, it->title);
        bool cur = app_is_current_context(it->uri);
        float tw = w - GRID_H - 24 - (cur ? 26 : 0);
        text_draw_vc(it->title, x + GRID_H + 12, ty + GRID_H / 2, FS_S, FW_BOLD,
                     cur ? rgb(C_ACCENT) : rgb(C_TEXT), tw);
        if (cur)
            gfx_equalizer(x + w - 18, ty + GRID_H / 2, 12, A.time, app_is_playing(), rgb(C_ACCENT));
        if (res.focused)
            draw_focus_ring(r, 4);
        if (res.clicked)
            activate(c, it, i);
        if (res.menu)
            app_open_sheet(it, c->context_uri, i);
    }
    uint32_t rows = (s->count + cols - 1) / cols;
    return y + rows * (GRID_H + GRID_GAP);
}

static float draw_shelf(sections_ctx *c, model_section *s, uint32_t si, float y)
{
    if (!near_view(c, y, CARD_H)) {
        return y + CARD_H;
    }
    scroller *sc = &c->v->shelves[si < MAX_SHELVES ? si : MAX_SHELVES - 1];
    /* Bleed to the content edges so cards scroll under the padding. */
    rectf vp = rect(CONTENT_X, y, CONTENT_W, CARD_H);
    float content = s->count * (CARD_W + CARD_GAP) - CARD_GAP + PAD * 2;
    ui_scroll_begin(sc, vp, content, AXIS_X, false);
    uint32_t base = ui_idx(ui_id("shelf"), si);
    for (uint32_t i = 0; i < s->count; i++) {
        float x = CONTENT_X + PAD + i * (CARD_W + CARD_GAP) - sc->pos;
        if (x + CARD_W < CONTENT_X - 400 || x > SCREEN_W + 400)
            continue;
        const vam_item *it = &s->items[i];
        rectf r = rect(x, y, CARD_W, CARD_H);
        ui_res res = ui_item(ui_idx(base, i), r, 0);
        if (x + CARD_W >= CONTENT_X && x <= SCREEN_W) {
            if (res.pressed || res.focused)
                gfx_rrect(r, 8, rgb(res.pressed ? C_SURFACE_HI2 : C_SURFACE_HI));
            bool artist = it->kind == VAM_KIND_ARTIST;
            rectf img = rect(x + 10, y + 10, CARD_IMG, CARD_IMG);
            if (!artist)
                gfx_shadow(img, 6, 6, 90);
            draw_cover(it->image_url, img, artist ? CARD_IMG / 2 : 6, it->kind, it->title);
            if (app_is_current_context(it->uri)) {
                gfx_circle(img.x + img.w - 22, img.y + img.h - 22, 18, rgb(C_ACCENT));
                gfx_equalizer(img.x + img.w - 22, img.y + img.h - 22, 14, A.time, app_is_playing(),
                              rgb(0x000000));
            }
            text_draw(it->title, x + 10, y + 152, FS_S, FW_BOLD, rgb(C_TEXT), CARD_IMG);
            text_draw(artist ? "Artist" : it->subtitle, x + 10, y + 174, FS_XS, FW_REGULAR,
                      rgb(C_SUBTEXT), CARD_IMG);
            if (res.focused)
                draw_focus_ring(r, 8);
        }
        if (res.clicked)
            activate(c, it, i);
        if (res.menu)
            app_open_sheet(it, c->context_uri, i);
    }
    ui_scroll_end();
    return y + CARD_H;
}

float draw_sections(sections_ctx *c, model_page *p, uint32_t first, float y)
{
    if (!p)
        return y;
    for (uint32_t i = first; i < p->section_count; i++) {
        model_section *s = &p->sections[i];
        if (s->count == 0)
            continue;
        if (s->title[0]) {
            y += 14;
            if (near_view(c, y, 36))
                text_draw(s->title, c->x, y, FS_XL, FW_BOLD, rgb(C_TEXT), c->w);
            y += 40;
        }
        switch (s->layout) {
        case VAM_LAYOUT_GRID: y = draw_grid(c, s, i, y); break;
        case VAM_LAYOUT_SHELF: y = draw_shelf(c, s, i, y); break;
        default: y = draw_list(c, s, i, y); break;
        }
        y += 8;
    }
    return y;
}
