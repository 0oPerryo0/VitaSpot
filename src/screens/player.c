/* Sidebar, bottom player bar and the Now Playing overlay. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../app.h"
#include "../gfx/images.h"
#include "../gfx/text.h"
#include "../theme.h"

/* How long a lifted finger may stay off a slider before the drag commits. */
#define SLIDER_RELEASE_GRACE_MS 120

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* hit_top > 0 keeps the touch area from reaching above that y, e.g. into
   buttons right above the bar. */
bool draw_slider(uint32_t id, rectf track, float *frac, float display_frac, float step, float hit_top)
{
    /* Tall touch area around a thin bar; nothing below the screen edge. */
    float hit_y = track.y + track.h / 2 - SLIDER_HIT_H / 2.0f;
    float hit_h = SLIDER_HIT_H;
    if (hit_top > hit_y) {
        hit_h -= hit_top - hit_y;
        hit_y = hit_top;
    }
    if (hit_y + hit_h > SCREEN_H)
        hit_h = SCREEN_H - hit_y;
    rectf hit = rect(track.x - 8, hit_y, track.w + 16, hit_h);
    ui_res res = ui_item(id, hit, UI_F_SLIDER);
    bool committed = false;

    if (A.slider_owner == 0 && ui_pointer_pressed_in(hit)) {
        A.slider_owner = id;
        A.slider_release_ms = 0;
    }
    if (A.slider_owner == id) {
        uint64_t now = vam_now_ms();
        if (ui_pointer_down() && (!A.slider_release_ms || ui_pointer_pressed_in(hit))) {
            *frac = clamp01((ui_pointer_x() - track.x) / track.w);
            A.slider_release_ms = 0;
        } else if (!ui_pointer_down() && !A.slider_release_ms) {
            /* The Vita panel can drop a finger for a frame or two mid-drag;
               wait briefly so a flicker doesn't commit a seek. */
            A.slider_release_ms = now ? now : 1;
        } else if (ui_pointer_down() || now - A.slider_release_ms >= SLIDER_RELEASE_GRACE_MS) {
            A.slider_owner = 0;
            A.slider_release_ms = 0;
            committed = true;
        }
    } else if (A.slider_owner == 0) {
        /* Sliders that mirror each other (bar and Now Playing) share *frac,
           so only reset it while nobody is dragging. */
        *frac = display_frac;
        if (res.slide) {
            *frac = clamp01(display_frac + res.slide * step);
            committed = true;
        }
    }

    bool active = A.slider_owner == id || res.focused;
    float f = clamp01(*frac);
    gfx_rrect(track, track.h / 2, rgba(0xFFFFFF, 60));
    gfx_rrect(rect(track.x, track.y, track.w * f, track.h), track.h / 2, active ? rgb(C_ACCENT) : rgb(0xFFFFFF));
    if (active)
        gfx_circle(track.x + track.w * f, track.y + track.h / 2, 7, rgb(0xFFFFFF));
    if (res.focused)
        draw_focus_ring(hit, 8);
    return committed;
}

static void seek_slider(uint32_t id, rectf track, bool times_below, float time_y, float hit_top)
{
    uint32_t dur = A.player.track.duration_ms;
    uint32_t pos = app_position_ms();
    float display = dur ? (float)pos / dur : 0;
    float step = dur ? 10000.0f / dur : 0;
    bool committed = draw_slider(id, track, &A.scrub_frac, display, step, hit_top);
    float shown = A.slider_owner == id ? A.scrub_frac : display;
    if (committed && dur)
        vam_seek((uint32_t)(A.scrub_frac * dur));

    char a[16], b[16];
    fmt_duration((uint32_t)(shown * dur), a, sizeof(a));
    fmt_duration(dur, b, sizeof(b));
    if (times_below) {
        text_draw(a, track.x, time_y, FS_XS, FW_REGULAR, rgb(C_SUBTEXT), 0);
        text_draw_aligned(b, track.x + track.w, time_y, FS_XS, FW_REGULAR, rgb(C_SUBTEXT), 0, ALIGN_RIGHT);
    } else {
        float cy = track.y + track.h / 2;
        text_draw_aligned(a, track.x - 10, cy - text_line_height(FS_XS) / 2.0f, FS_XS, FW_REGULAR,
                          rgb(C_SUBTEXT), 0, ALIGN_RIGHT);
        text_draw(b, track.x + track.w + 10, cy - text_line_height(FS_XS) / 2.0f, FS_XS, FW_REGULAR,
                  rgb(C_SUBTEXT), 0);
    }
}

static void volume_slider(uint32_t id, rectf track)
{
    float display = A.player.volume / 65535.0f;
    if (draw_slider(id, track, &A.vol_frac, display, 0.08f, 0))
        vam_set_volume((uint16_t)(clamp01(A.vol_frac) * 65535));
    else if (A.slider_owner == id)
        vam_set_volume((uint16_t)(clamp01(A.vol_frac) * 65535));  /* live while dragging */
}

static void cycle_repeat(void)
{
    int next = A.player.repeat == VAM_REPEAT_OFF ? VAM_REPEAT_CONTEXT
             : A.player.repeat == VAM_REPEAT_CONTEXT ? VAM_REPEAT_TRACK
             : VAM_REPEAT_OFF;
    vam_set_repeat(next);
}

static void toggle_play(void)
{
    if (app_is_playing())
        vam_pause();
    else
        vam_resume();
}

/* Shuffle / prev / play / next / repeat, centred on cx. */
static void transport(const char *prefix, float cx, float cy, float gap, float play_r, float icon)
{
    char id[48];
    color on = rgb(C_ACCENT), off = rgb(C_SUBTEXT);

    snprintf(id, sizeof(id), "%s.shuffle", prefix);
    if (icon_button(ui_id(id), ICON_SHUFFLE, cx - gap * 2, cy, icon * 0.8f, icon + 16, A.player.shuffle ? on : off))
        vam_set_shuffle(!A.player.shuffle);
    if (A.player.shuffle)
        gfx_circle(cx - gap * 2, cy + icon * 0.75f, 2, on);

    snprintf(id, sizeof(id), "%s.prev", prefix);
    if (icon_button(ui_id(id), ICON_PREV, cx - gap, cy, icon, icon + 16, rgb(C_TEXT)))
        vam_prev();

    snprintf(id, sizeof(id), "%s.play", prefix);
    if (A.player.playback == VAM_PLAYBACK_BUFFERING) {
        gfx_circle(cx, cy, play_r, rgb(0xFFFFFF));
        gfx_spinner(cx, cy, play_r * 0.5f, A.time, rgb(0x000000));
        if (ui_item(ui_id(id), rect(cx - play_r, cy - play_r, play_r * 2, play_r * 2), 0).clicked)
            vam_pause();
    } else if (round_button(ui_id(id), app_is_playing() ? ICON_PAUSE : ICON_PLAY, cx, cy, play_r, rgb(0xFFFFFF),
                            rgb(0x000000))) {
        toggle_play();
    }

    snprintf(id, sizeof(id), "%s.next", prefix);
    if (icon_button(ui_id(id), ICON_NEXT, cx + gap, cy, icon, icon + 16, rgb(C_TEXT)))
        vam_next();

    snprintf(id, sizeof(id), "%s.repeat", prefix);
    bool rep = A.player.repeat != VAM_REPEAT_OFF;
    if (icon_button(ui_id(id), ICON_REPEAT, cx + gap * 2, cy, icon * 0.8f, icon + 16, rep ? on : off))
        cycle_repeat();
    if (rep)
        gfx_circle(cx + gap * 2, cy + icon * 0.75f, 2, on);
    if (A.player.repeat == VAM_REPEAT_TRACK) {
        gfx_circle(cx + gap * 2 + icon * 0.4f, cy - icon * 0.4f, 6, on);
        text_draw_aligned("1", cx + gap * 2 + icon * 0.4f, cy - icon * 0.4f - text_line_height(FS_XS) / 2.0f,
                          FS_XS, FW_BOLD, rgb(0x000000), 0, ALIGN_CENTER);
    }
}

/* ------------------------------------------------------------------------ */

void screen_sidebar(void)
{
    static const char *const labels[TAB_COUNT] = { "Home", "Search", "Library" };
    static const icon_id icons[TAB_COUNT][2] = {
        { ICON_HOME, ICON_HOME_FILLED },
        { ICON_SEARCH, ICON_SEARCH_BOLD },
        { ICON_LIBRARY, ICON_LIBRARY_BOLD },
    };
    gfx_rect(rect(0, 0, SIDEBAR_W, SCREEN_H - PLAYER_H), rgb(C_BG_DEEP));

    /* The signed-in user's profile picture; until it loads (or without
       one), the app mark: a generic note glyph, not anyone's logo. */
    rectf mark = rect(SIDEBAR_W / 2.0f - 18, 16, 36, 36);
    SDL_Texture *avatar = images_get(A.auth.image_url, NULL);
    if (avatar) {
        gfx_image(avatar, mark, mark.w / 2, 255);
    } else {
        gfx_rrect(mark, 10, rgb(C_ACCENT));
        gfx_icon(ICON_NOTE, SIDEBAR_W / 2.0f, 34, 24, rgb(0x000000));
    }

    for (int i = 0; i < TAB_COUNT; i++) {
        rectf r = rect(6, 78 + i * 72.0f, SIDEBAR_W - 12, 64);
        ui_res res = ui_item(ui_idx(ui_id("tab"), i), r, 0);
        bool active = A.tab == i;
        color c = active ? rgb(C_TEXT) : rgb(res.pressed ? C_TEXT : C_SUBTEXT);
        if (res.focused)
            gfx_rrect(r, 8, rgb(C_SURFACE_HI));
        gfx_icon(icons[i][active], r.x + r.w / 2, r.y + 23, 26, c);
        text_draw_aligned(labels[i], r.x + r.w / 2, r.y + 40, FS_XS, active ? FW_BOLD : FW_REGULAR, c, r.w,
                          ALIGN_CENTER);
        if (res.focused)
            draw_focus_ring(r, 8);
        if (res.clicked)
            app_switch_tab(i);
    }
}

void screen_player_bar(void)
{
    float y = SCREEN_H - PLAYER_H;
    gfx_rect(rect(0, y, SCREEN_W, PLAYER_H), rgb(C_BG_DEEP));
    gfx_rect(rect(0, y, SCREEN_W, 1), rgb(C_DIVIDER));
    if (!A.player.has_track) {
        text_draw_vc("Pick something to play", 20, y + PLAYER_H / 2.0f, FS_S, FW_REGULAR, rgb(C_MUTED), 300);
        return;
    }
    const vam_item *t = &A.player.track;

    /* Left: art + title opens Now Playing. */
    rectf left = rect(8, y + 8, 248, PLAYER_H - 16);
    ui_res res = ui_item(ui_id("bar.track"), left, 0);
    if (res.pressed || res.focused)
        gfx_rrect(left, 6, rgb(C_SURFACE_HI));
    draw_cover(t->image_url, rect(14, y + 16, 52, 52), 4, t->kind, t->title);
    text_draw(t->title, 78, y + 20, FS_S, FW_BOLD, rgb(C_TEXT), 170);
    text_draw(t->subtitle, 78, y + 44, FS_XS, FW_REGULAR, rgb(C_SUBTEXT), 170);
    if (res.focused)
        draw_focus_ring(left, 6);
    if (res.clicked)
        app_set_now_playing(true);

    if (icon_button(ui_id("bar.like"), t->is_saved ? ICON_HEART : ICON_HEART_OUTLINE, 284, y + PLAYER_H / 2.0f,
                    20, 48, t->is_saved ? rgb(C_ACCENT) : rgb(C_SUBTEXT)))
        app_toggle_saved(t->uri, t->is_saved);

    /* Centre: transport + seek. */
    /* Transport hits end at y + 54, where the seek hit begins; the seek hit
       then runs to the screen edge. */
    transport("bar", 500, y + 30, 46, 20, 22);
    seek_slider(ui_id("bar.seek"), rect(358, y + 66, 284, 4), false, 0, y + 54);

    /* Right: device / queue / volume. */
    if (!A.player.is_active_device)
        gfx_icon(ICON_DEVICE, 712, y + PLAYER_H / 2.0f, 22, rgb(C_ACCENT));
    if (icon_button(ui_id("bar.queue"), ICON_QUEUE, 752, y + PLAYER_H / 2.0f, 20, 48, rgb(C_SUBTEXT)))
        app_open_uri("vam:queue");
    gfx_icon(ICON_VOLUME, 794, y + PLAYER_H / 2.0f, 20, rgb(C_SUBTEXT));
    volume_slider(ui_id("bar.volume"), rect(816, y + PLAYER_H / 2.0f - 2, 124, 4));
}

/* ------------------------------------------------------------------------ */

void screen_now_playing(void)
{
    float t = A.np_anim;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);
    float oy = (1 - e) * SCREEN_H;
    const vam_item *tr = &A.player.track;

    color target = rgb(0x404040);
    images_get(tr->image_url, &target);
    A.np_color = color_lerp(A.np_color, target, A.dt * 4 > 1 ? 1 : A.dt * 4);

    gfx_rect(rect(0, oy, SCREEN_W, SCREEN_H), rgb(C_BG));
    gfx_vgradient(rect(0, oy, SCREEN_W, SCREEN_H * 0.85f), color_scale(A.np_color, 1.1f), rgb(C_BG));
    /* Block everything underneath. */
    ui_item(ui_id("np.backdrop"), rect(0, oy, SCREEN_W, SCREEN_H), UI_F_NOFOCUS);

    if (icon_button(ui_id("np.close"), ICON_CHEVRON_DOWN, 36, oy + 32, 28, 52, rgb(C_TEXT)))
        app_set_now_playing(false);
    char from[64];
    const char *k = "";
    if (strncmp(A.player.context_uri, "spotify:album:", 14) == 0) k = "ALBUM";
    else if (strncmp(A.player.context_uri, "spotify:playlist:", 17) == 0) k = "PLAYLIST";
    else if (strncmp(A.player.context_uri, "spotify:artist:", 15) == 0) k = "ARTIST";
    else if (strstr(A.player.context_uri, "collection")) k = "YOUR LIBRARY";
    if (A.player.context_title[0]) {
        snprintf(from, sizeof(from), k[0] ? "PLAYING FROM %s" : "PLAYING FROM", k);
        text_draw_aligned(from, SCREEN_W / 2.0f, oy + 12, FS_XS, FW_REGULAR, rgb(C_TEXT), 500, ALIGN_CENTER);
        text_draw_aligned(A.player.context_title, SCREEN_W / 2.0f, oy + 32, FS_S, FW_BOLD, rgb(C_TEXT), 500,
                          ALIGN_CENTER);
    }
    if (icon_button(ui_id("np.queue"), ICON_QUEUE, SCREEN_W - 92, oy + 32, 24, 52, rgb(C_TEXT)))
        app_open_uri("vam:queue");
    if (icon_button(ui_id("np.more"), ICON_MORE, SCREEN_W - 36, oy + 32, 24, 52, rgb(C_TEXT)))
        app_open_sheet(tr, A.player.context_uri, 0);

    /* Artwork */
    rectf art = rect(64, oy + 82, 360, 360);
    gfx_shadow(art, 8, 22, 200);
    draw_cover(tr->image_url, art, 8, tr->kind, tr->title);

    /* Track info */
    float x0 = 480, w = SCREEN_W - x0 - 56;
    text_draw(tr->title, x0, oy + 150, FS_XXL, FW_BOLD, rgb(C_TEXT), w - 50);
    rectf artist_r = rect(x0, oy + 192, w - 50, 32);
    ui_res ar = ui_item(ui_id("np.artist"), artist_r, 0);
    text_draw(tr->subtitle, x0, oy + 196, FS_L, FW_REGULAR, rgb(ar.pressed ? C_TEXT : C_SUBTEXT), w - 50);
    if (ar.focused)
        draw_focus_ring(artist_r, 4);
    if (ar.clicked && tr->artist_uri[0])
        app_open_uri(tr->artist_uri);
    if (icon_button(ui_id("np.like"), tr->is_saved ? ICON_HEART : ICON_HEART_OUTLINE, x0 + w - 14, oy + 186, 26,
                    52, tr->is_saved ? rgb(C_ACCENT) : rgb(C_TEXT)))
        app_toggle_saved(tr->uri, tr->is_saved);

    seek_slider(ui_id("np.seek"), rect(x0, oy + 262, w, 5), true, oy + 276, 0);
    transport("np", x0 + w / 2, oy + 346, 72, 32, 30);

    gfx_icon(ICON_VOLUME, x0 + 10, oy + 420, 20, rgb(C_SUBTEXT));
    volume_slider(ui_id("np.volume"), rect(x0 + 36, oy + 418, w - 36, 4));

    if (!A.player.is_active_device) {
        gfx_icon(ICON_DEVICE, x0 + 10, oy + 462, 20, rgb(C_ACCENT));
        text_draw_vc("Playing on another device", x0 + 30, oy + 462, FS_S, FW_BOLD, rgb(C_ACCENT), w);
    }
}
