#include <stdio.h>

#include "../app.h"
#include "../gfx/images.h"
#include "../gfx/text.h"
#include "../theme.h"

void fmt_duration(uint32_t ms, char *buf, size_t n)
{
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(buf, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(buf, n, "%u:%02u", s / 60, s % 60);
}

const char *kind_label(int kind)
{
    switch (kind) {
    case VAM_KIND_TRACK: return "Song";
    case VAM_KIND_EPISODE: return "Episode";
    case VAM_KIND_ALBUM: return "Album";
    case VAM_KIND_PLAYLIST: return "Playlist";
    case VAM_KIND_ARTIST: return "Artist";
    case VAM_KIND_SHOW: return "Podcast";
    case VAM_KIND_COLLECTION: return "Playlist";
    default: return "";
    }
}

void draw_cover(const char *url, rectf r, float radius, int kind, const char *title)
{
    (void)title;
    SDL_Texture *t = images_get(url, NULL);
    if (t) {
        gfx_image(t, r, radius, 255);
        return;
    }
    float s = r.w < r.h ? r.w : r.h;
    if (kind == VAM_KIND_COLLECTION) {
        /* Liked Songs tile: own gradient + heart. */
        gfx_rrect(r, radius, rgb(C_LIKED_TOP));
        gfx_vgradient(rect_inset(r, radius > 2 ? radius * 0.3f : 0), rgba(C_LIKED_TOP, 0),
                      rgba(C_LIKED_BOT, 180));
        gfx_icon(ICON_HEART, r.x + r.w / 2, r.y + r.h / 2, s * 0.42f, rgb(0xFFFFFF));
        return;
    }
    gfx_rrect(r, radius, rgb(C_SURFACE_HI));
    icon_id ic = ICON_NOTE;
    if (kind == VAM_KIND_ARTIST) ic = ICON_PERSON;
    else if (kind == VAM_KIND_ALBUM) ic = ICON_DISC;
    gfx_icon(ic, r.x + r.w / 2, r.y + r.h / 2, s * 0.4f, rgb(C_MUTED));
}

void draw_focus_ring(rectf r, float radius)
{
    gfx_rrect_outline(rect_inset(r, -3), radius + 3, 2.5f, rgb(0xFFFFFF));
}

bool icon_button(uint32_t id, icon_id icon, float cx, float cy, float size, float hit, color c)
{
    if (hit < TOUCH_MIN)
        hit = TOUCH_MIN;
    rectf r = rect(cx - hit / 2, cy - hit / 2, hit, hit);
    ui_res res = ui_item(id, r, 0);
    float s = res.pressed ? size * 0.9f : size;
    if (res.pressed)
        c = with_alpha(c, 0.7f);
    if (res.focused)
        gfx_circle(cx, cy, hit / 2, rgba(0xFFFFFF, 40));
    gfx_icon(icon, cx, cy, s, c);
    if (res.focused)
        draw_focus_ring(r, hit / 2);
    return res.clicked;
}

bool round_button(uint32_t id, icon_id icon, float cx, float cy, float radius, color bg, color fg)
{
    rectf r = rect(cx - radius, cy - radius, radius * 2, radius * 2);
    ui_res res = ui_item(id, r, 0);
    float k = res.pressed ? 0.94f : 1.0f;
    gfx_circle(cx, cy, radius * k, res.pressed ? color_scale(bg, 0.85f) : bg);
    /* Play triangles look centred when nudged right. */
    float nudge = icon == ICON_PLAY ? radius * 0.08f : 0;
    gfx_icon(icon, cx + nudge, cy, radius * 0.95f * k, fg);
    if (res.focused)
        draw_focus_ring(r, radius);
    return res.clicked;
}

bool pill_button(uint32_t id, const char *label, float x, float y, bool active, float *out_w)
{
    float w = text_width(label, FS_S, FW_REGULAR, 0) + 32.0f, h = 40;
    rectf r = rect(x, y, w, h);
    ui_res res = ui_item(id, r, 0);
    color bg = active ? rgb(C_ACCENT) : rgb(res.pressed ? C_SURFACE_HI : C_SURFACE_HI2);
    color fg = active ? rgb(0x000000) : rgb(C_TEXT);
    gfx_rrect(r, h / 2, bg);
    text_draw_vc(label, x + 16, y + h / 2, FS_S, FW_REGULAR, fg, 0);
    if (res.focused)
        draw_focus_ring(r, h / 2);
    if (out_w)
        *out_w = w;
    return res.clicked;
}

void draw_back_button(view *v, float x, float y)
{
    (void)v;
    float r = 20;
    rectf hit = rect(x - 4, y - 4, r * 2 + 8, r * 2 + 8);
    ui_res res = ui_item(ui_id("nav.back"), hit, 0);
    gfx_circle(x + r, y + r, r, rgba(0x000000, res.pressed ? 200 : 150));
    gfx_icon(ICON_BACK, x + r - 1, y + r, 22, rgb(0xFFFFFF));
    if (res.focused)
        draw_focus_ring(rect(x, y, r * 2, r * 2), r);
    if (res.clicked)
        app_go_back();
}

void draw_loading(rectf area)
{
    gfx_spinner(area.x + area.w / 2, area.y + area.h / 2, 18, A.time, rgb(C_SUBTEXT));
}

bool draw_error(rectf area, const char *msg)
{
    float cy = area.y + area.h / 2;
    text_draw_aligned("Couldn't load this", area.x + area.w / 2, cy - 62, FS_L, FW_BOLD,
                      rgb(C_TEXT), area.w - 40, ALIGN_CENTER);
    text_draw_aligned(msg && *msg ? msg : "Check your connection and try again.", area.x + area.w / 2,
                      cy - 30, FS_S, FW_REGULAR, rgb(C_SUBTEXT), area.w - 40, ALIGN_CENTER);
    float w = text_width("Try again", FS_S, FW_REGULAR, 0) + 32.0f;
    return pill_button(ui_id("error.retry"), "Try again", area.x + (area.w - w) / 2, cy + 6, false, NULL);
}
