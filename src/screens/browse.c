/* Home, Search and Library tab roots. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../app.h"
#include "../gfx/text.h"
#include "../theme.h"

static rectf content_rect(void) { return rect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H); }

/* Shared loading / error handling. Returns true if the page is ready. */
static bool ensure_page(view *v, rectf area)
{
    if (!v->page && !v->req && !v->failed)
        app_request_view(v);
    if (v->page)
        return true;
    if (v->failed) {
        if (draw_error(area, v->error))
            app_request_view(v);
    } else {
        draw_loading(area);
    }
    return false;
}

static const char *greeting(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int h = t ? t->tm_hour : 12;
    if (h >= 5 && h < 12) return "Good morning";
    if (h >= 12 && h < 18) return "Good afternoon";
    return "Good evening";
}

void screen_home(view *v)
{
    rectf vp = content_rect();
    gfx_vgradient(rect(CONTENT_X, 0, CONTENT_W, 280), rgb(0x1E3326), rgb(C_BG));
    if (!ensure_page(v, vp))
        return;

    ui_scroll_begin(&v->scroll, vp, v->content_h, AXIS_Y, true);
    float y = CONTENT_Y + 22 - v->scroll.pos;
    text_draw(greeting(), CONTENT_X + PAD, y, FS_XXL, FW_BOLD, rgb(C_TEXT), CONTENT_W - 2 * PAD);
    y += 52;
    sections_ctx c = { v, NULL, false, true, vp, CONTENT_X + PAD - ROW_INSET, CONTENT_W - 2 * PAD + 2 * ROW_INSET };
    y = draw_sections(&c, v->page, 0, y);
    v->content_h = y + v->scroll.pos - CONTENT_Y + 24;
    ui_scroll_end();
}

/* ------------------------------------------------------------------------ */

static void search_bar(view *v)
{
    rectf r = rect(CONTENT_X + PAD, 12, CONTENT_W - 2 * PAD, 48);
    ui_res res = ui_item(ui_id("search.bar"), r, 0);
    gfx_rrect(r, 24, rgb(res.pressed ? 0xE6E6E6 : 0xFFFFFF));
    gfx_icon(ICON_SEARCH_BOLD, r.x + 26, r.y + r.h / 2, 22, rgb(0x121212));
    float tx = r.x + 50, tw = r.w - 100;
    const char *shown = A.typing ? A.edit : v->query;
    if (shown[0]) {
        int w = text_draw_vc(shown, tx, r.y + r.h / 2, FS_M, FW_REGULAR, rgb(0x121212), tw);
        if (A.typing && ((int)(A.time * 2) % 2 == 0))
            gfx_rect(rect(tx + w + 2, r.y + 12, 2, r.h - 24), rgb(0x121212));
    } else {
        text_draw_vc("What do you want to listen to?", tx, r.y + r.h / 2, FS_M, FW_REGULAR, rgb(0x6A6A6A), tw);
        if (A.typing && ((int)(A.time * 2) % 2 == 0))
            gfx_rect(rect(tx, r.y + 12, 2, r.h - 24), rgb(0x121212));
    }
    if (res.focused)
        draw_focus_ring(r, 24);
    if (res.clicked && !A.typing)
        app_begin_typing();

    if (shown[0] && !A.typing) {
        if (icon_button(ui_id("search.clear"), ICON_CLOSE, r.x + r.w - 26, r.y + r.h / 2, 20, TOUCH_MIN, rgb(0x121212)))
            app_set_search("");
    }
}

static void recent_searches(view *v, rectf vp)
{
    ui_scroll_begin(&v->scroll, vp, v->content_h, AXIS_Y, true);
    float y = vp.y + 8 - v->scroll.pos;
    float x = CONTENT_X + PAD, w = CONTENT_W - 2 * PAD;
    if (!A.recent[0][0]) {
        float cy = vp.y + vp.h / 2 - 30;
        gfx_icon(ICON_SEARCH, CONTENT_X + CONTENT_W / 2.0f, cy - 36, 44, rgb(C_MUTED));
        text_draw_aligned("Play what you love", CONTENT_X + CONTENT_W / 2.0f, cy, FS_L, FW_BOLD,
                          rgb(C_TEXT), 0, ALIGN_CENTER);
        text_draw_aligned("Search for songs, artists, albums and playlists.", CONTENT_X + CONTENT_W / 2.0f,
                          cy + 32, FS_S, FW_REGULAR, rgb(C_SUBTEXT), 0, ALIGN_CENTER);
        v->content_h = 0;
        ui_scroll_end();
        return;
    }
    text_draw("Recent searches", x, y, FS_L, FW_BOLD, rgb(C_TEXT), w);
    y += 40;
    for (int i = 0; i < RECENT_SEARCHES && A.recent[i][0]; i++) {
        rectf r = rect(x, y, w, 56);
        ui_res res = ui_item(ui_idx(ui_id("recent"), i), r, 0);
        if (res.pressed || res.focused)
            gfx_rrect(r, 6, rgb(res.pressed ? C_SURFACE_HI2 : C_SURFACE_HI));
        gfx_icon(ICON_SEARCH, x + 22, y + 28, 20, rgb(C_SUBTEXT));
        text_draw_vc(A.recent[i], x + 50, y + 28, FS_M, FW_REGULAR, rgb(C_TEXT), w - 60);
        if (res.focused)
            draw_focus_ring(r, 6);
        if (res.clicked) {
            char q[256];
            snprintf(q, sizeof(q), "%s", A.recent[i]);
            app_set_search(q);
        }
        y += 56;
    }
    v->content_h = y + v->scroll.pos - vp.y + 16;
    ui_scroll_end();
}

void screen_search(view *v)
{
    rectf vp = rect(CONTENT_X, 72, CONTENT_W, CONTENT_H - 72);
    if (!v->query[0]) {
        recent_searches(v, vp);
    } else if (ensure_page(v, vp)) {
        gfx_push_clip(vp);
        ui_push_clip(vp);
        ui_scroll_begin(&v->scroll, vp, v->content_h, AXIS_Y, true);
        float y = vp.y - v->scroll.pos;
        sections_ctx c = { v, NULL, false, true, vp, CONTENT_X + PAD - ROW_INSET, CONTENT_W - 2 * PAD + 2 * ROW_INSET };
        y = draw_sections(&c, v->page, 0, y);
        bool empty = true;
        for (uint32_t i = 0; i < v->page->section_count; i++)
            if (v->page->sections[i].count)
                empty = false;
        if (empty) {
            char msg[320];
            snprintf(msg, sizeof(msg), "No results found for \"%s\"", v->query);
            text_draw_aligned(msg, CONTENT_X + CONTENT_W / 2.0f, vp.y + 80, FS_L, FW_BOLD, rgb(C_TEXT),
                              CONTENT_W - 2 * PAD, ALIGN_CENTER);
            text_draw_aligned("Check the spelling, or try different keywords.", CONTENT_X + CONTENT_W / 2.0f,
                              vp.y + 114, FS_S, FW_REGULAR, rgb(C_SUBTEXT), 0, ALIGN_CENTER);
        }
        v->content_h = y + v->scroll.pos - vp.y + 24;
        ui_scroll_end();
        ui_pop_clip();
        gfx_pop_clip();
    }
    /* Header on top, so it can't be covered by scrolled content. */
    gfx_rect(rect(CONTENT_X, 0, CONTENT_W, 72), rgb(C_BG));
    search_bar(v);
}

/* ------------------------------------------------------------------------ */

static bool lib_match(int filter, int kind)
{
    switch (filter) {
    case LIB_PLAYLISTS: return kind == VAM_KIND_PLAYLIST || kind == VAM_KIND_COLLECTION;
    case LIB_ALBUMS: return kind == VAM_KIND_ALBUM;
    case LIB_ARTISTS: return kind == VAM_KIND_ARTIST;
    default: return true;
    }
}

void screen_library(view *v)
{
    static const char *const labels[LIB_FILTER_COUNT] = { "All", "Playlists", "Albums", "Artists" };
    rectf vp = rect(CONTENT_X, 104, CONTENT_W, CONTENT_H - 104);

    if (ensure_page(v, vp) && v->page->section_count) {
        gfx_push_clip(vp);
        ui_push_clip(vp);
        ui_scroll_begin(&v->scroll, vp, v->content_h, AXIS_Y, true);
        float y = vp.y + 4 - v->scroll.pos;
        sections_ctx c = { v, NULL, false, true, vp, CONTENT_X + PAD - ROW_INSET, CONTENT_W - 2 * PAD + 2 * ROW_INSET };
        model_section *s = &v->page->sections[0];
        uint32_t base = ui_id("library.rows");
        int shown = 0;
        for (uint32_t i = 0; i < s->count; i++) {
            if (!lib_match(v->filter, s->items[i].kind))
                continue;
            y = draw_entity_row(&c, &s->items[i], i, y, base);
            shown++;
        }
        if (!shown)
            text_draw_aligned("Nothing here yet", CONTENT_X + CONTENT_W / 2.0f, vp.y + 60, FS_M, FW_BOLD,
                              rgb(C_SUBTEXT), 0, ALIGN_CENTER);
        v->content_h = y + v->scroll.pos - vp.y + 16;
        ui_scroll_end();
        ui_pop_clip();
        gfx_pop_clip();
    }

    gfx_rect(rect(CONTENT_X, 0, CONTENT_W, 104), rgb(C_BG));
    text_draw("Your Library", CONTENT_X + PAD, 18, FS_XL, FW_BOLD, rgb(C_TEXT), CONTENT_W - 2 * PAD);
    float x = CONTENT_X + PAD;
    for (int f = 1; f < LIB_FILTER_COUNT; f++) {
        float w = 0;
        bool active = v->filter == f;
        if (pill_button(ui_idx(ui_id("library.filter"), f), labels[f], x, 56, active, &w)) {
            v->filter = active ? LIB_ALL : f;
            v->scroll.pos = 0;
        }
        x += w + 8;
    }
    if (v->scroll.pos > 2)
        gfx_rect(rect(CONTENT_X, 103, CONTENT_W, 1), rgb(C_DIVIDER));
}
