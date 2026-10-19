/* Album / playlist / artist / Liked Songs / queue detail page. */

#include <stdio.h>
#include <string.h>

#include "../app.h"
#include "../gfx/images.h"
#include "../gfx/text.h"
#include "../theme.h"

#define COVER 168
#define HEADER_TOP 56

static vam_item page_as_item(const model_page *p)
{
    vam_item it;
    memset(&it, 0, sizeof(it));
    it.kind = p->kind;
    snprintf(it.uri, sizeof(it.uri), "%s", p->uri);
    snprintf(it.title, sizeof(it.title), "%s", p->title);
    snprintf(it.subtitle, sizeof(it.subtitle), "%s", p->subtitle);
    snprintf(it.image_url, sizeof(it.image_url), "%s", p->image_url);
    it.is_saved = p->is_saved;
    it.is_playable = 1;
    return it;
}

static bool can_save(int kind)
{
    return kind == VAM_KIND_ALBUM || kind == VAM_KIND_PLAYLIST || kind == VAM_KIND_ARTIST ||
           kind == VAM_KIND_SHOW;
}

static void play_page(const model_page *p)
{
    if (app_is_current_context(p->uri)) {
        if (app_is_playing())
            vam_pause();
        else
            vam_resume();
        return;
    }
    vam_item it = page_as_item(p);
    app_play_item(&it, NULL, 0);
}

static void action_row(view *v, float x, float y, color avg)
{
    (void)avg;
    model_page *p = v->page;
    bool playing_here = app_is_current_context(p->uri) && app_is_playing();
    if (round_button(ui_id("page.play"), playing_here ? ICON_PAUSE : ICON_PLAY, x + 28, y + 28, 28,
                     rgb(C_ACCENT), rgb(0x000000)))
        play_page(p);
    float cx = x + 88;
    color sh = A.player.shuffle ? rgb(C_ACCENT) : rgb(C_SUBTEXT);
    if (icon_button(ui_id("page.shuffle"), ICON_SHUFFLE, cx, y + 28, 26, 44, sh))
        vam_set_shuffle(!A.player.shuffle);
    if (A.player.shuffle)
        gfx_circle(cx, y + 48, 2, rgb(C_ACCENT));
    cx += 52;
    if (p->kind == VAM_KIND_ARTIST) {
        float w = 0;
        if (pill_button(ui_id("page.follow"), p->is_saved ? "Following" : "Follow", cx - 16, y + 12, false, &w))
            app_toggle_saved(p->uri, p->is_saved);
        cx += w + 16;
    } else if (can_save(p->kind)) {
        if (icon_button(ui_id("page.save"), p->is_saved ? ICON_HEART : ICON_HEART_OUTLINE, cx, y + 28, 26, 44,
                        p->is_saved ? rgb(C_ACCENT) : rgb(C_SUBTEXT)))
            app_toggle_saved(p->uri, p->is_saved);
        cx += 52;
    }
    if (p->kind != VAM_KIND_QUEUE && p->kind != VAM_KIND_COLLECTION) {
        if (icon_button(ui_id("page.more"), ICON_MORE, cx, y + 28, 24, 44, rgb(C_SUBTEXT))) {
            vam_item it = page_as_item(p);
            app_open_sheet(&it, NULL, 0);
        }
    }
}

static int title_size(const char *title, float w)
{
    if (text_width(title, FS_HUGE, FW_BOLD, 0) <= w) return FS_HUGE;
    if (text_width(title, FS_XXL, FW_BOLD, 0) <= w) return FS_XXL;
    return FS_XL;
}

static float draw_header(view *v, float y0, color avg)
{
    model_page *p = v->page;
    bool artist = p->kind == VAM_KIND_ARTIST;
    float cx = CONTENT_X + PAD + 4;
    float top = y0 + HEADER_TOP;
    rectf cover = rect(cx, top, COVER, COVER);
    if (!artist)
        gfx_shadow(cover, 4, 14, 160);
    draw_cover(p->image_url, cover, artist ? COVER / 2.0f : 4, p->kind, p->title);

    float tx = cx + COVER + 24;
    float tw = CONTENT_X + CONTENT_W - PAD - tx;
    const char *label = p->kind == VAM_KIND_COLLECTION ? "Playlist" : kind_label(p->kind);
    float ty = top + 30;
    text_draw(label, tx, ty, FS_S, FW_BOLD, rgb(C_TEXT), tw);
    int ts = title_size(p->title, tw);
    text_draw(p->title, tx, ty + 20, ts, FW_BOLD, rgb(C_TEXT), tw);
    float by = top + COVER - 22;
    if (p->description[0])
        text_draw_wrapped(p->description, tx, by - 40, FS_S, FW_REGULAR, with_alpha(rgb(C_TEXT), 0.7f), tw, 2);
    text_draw(p->subtitle, tx, by, FS_S, FW_REGULAR, rgb(C_TEXT), tw);
    (void)avg;
    return top + COVER + 18;
}

static void sticky_bar(view *v, color avg)
{
    float t = (v->scroll.pos - 180) / 60.0f;
    if (t <= 0)
        return;
    if (t > 1) t = 1;
    rectf bar = rect(CONTENT_X, 0, CONTENT_W, 60);
    ui_item(ui_id("page.sticky"), bar, UI_F_NOFOCUS);  /* swallow taps */
    gfx_rect(bar, with_alpha(color_scale(avg, 0.55f), t));
    if (t > 0.5f) {
        float a = (t - 0.5f) * 2;
        gfx_circle(CONTENT_X + 76, 30, 18, with_alpha(rgb(C_ACCENT), a));
        bool playing_here = app_is_current_context(v->page->uri) && app_is_playing();
        gfx_icon(playing_here ? ICON_PAUSE : ICON_PLAY, CONTENT_X + 77, 30, 18, with_alpha(rgb(0x000000), a));
        rectf hit = rect(CONTENT_X + 56, 10, 40, 40);
        if (ui_item(ui_id("page.sticky.play"), hit, UI_F_NOFOCUS).clicked)
            play_page(v->page);
        text_draw_vc(v->page->title, CONTENT_X + 106, 30, FS_L, FW_BOLD, with_alpha(rgb(C_TEXT), a),
                     CONTENT_W - 140);
    }
}

void screen_page(view *v)
{
    rectf vp = rect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H);
    if (!v->page && !v->req && !v->failed)
        app_request_view(v);
    if (!v->page) {
        if (v->failed) {
            if (draw_error(vp, v->error))
                app_request_view(v);
        } else {
            draw_loading(vp);
        }
        draw_back_button(v, CONTENT_X + 14, 12);
        return;
    }
    model_page *p = v->page;

    /* The queue follows the player; refresh it when playback changes. */
    if (p->kind == VAM_KIND_QUEUE && v->player_seq != A.player.seq && !v->req) {
        v->player_seq = A.player.seq;
        app_request_view(v);
    }

    color avg = rgb(0x535353);
    images_get(p->image_url, &avg);
    bool queue = p->kind == VAM_KIND_QUEUE;
    if (p->kind == VAM_KIND_COLLECTION && !p->image_url[0])
        avg = color_scale(rgb(C_LIKED_TOP), 0.8f);

    ui_scroll_begin(&v->scroll, vp, v->content_h, AXIS_Y, true);
    float y0 = CONTENT_Y - v->scroll.pos;
    if (y0 > 0)
        gfx_rect(rect(CONTENT_X, 0, CONTENT_W, y0), avg);
    gfx_vgradient(rect(CONTENT_X, y0, CONTENT_W, 300), avg, color_scale(avg, 0.35f));
    gfx_vgradient(rect(CONTENT_X, y0 + 300, CONTENT_W, 160), color_scale(avg, 0.35f), rgb(C_BG));

    float y;
    if (queue) {
        text_draw("Queue", CONTENT_X + PAD + 50, y0 + 16, FS_XL, FW_BOLD, rgb(C_TEXT), CONTENT_W);
        y = y0 + 62;
    } else {
        y = draw_header(v, y0, avg);
        action_row(v, CONTENT_X + PAD, y, avg);
        y += 70;
    }

    bool numbered = p->kind == VAM_KIND_ALBUM || p->kind == VAM_KIND_ARTIST;
    sections_ctx c = { v, queue ? NULL : p->uri, numbered, !numbered, vp, CONTENT_X + PAD - 8,
                       CONTENT_W - 2 * PAD + 16 };
    float before = y;
    y = draw_sections(&c, p, 0, y);
    if (y == before && !v->req)
        text_draw_aligned(queue ? "Your queue is empty" : "Nothing here yet", CONTENT_X + CONTENT_W / 2.0f,
                          y + 30, FS_M, FW_BOLD, rgb(C_SUBTEXT), 0, ALIGN_CENTER);
    v->content_h = y + v->scroll.pos - CONTENT_Y + 24;
    ui_scroll_end();

    if (!queue)
        sticky_bar(v, avg);
    draw_back_button(v, CONTENT_X + 14, 12);
}
