/* Item context menu (long-press / Square / "..." button). */

#include <stdio.h>
#include <string.h>

#include "../app.h"
#include "../gfx/text.h"
#include "../theme.h"

typedef enum act { ACT_PLAY, ACT_SAVE, ACT_QUEUE, ACT_ALBUM, ACT_ARTIST } act;

typedef struct option { act a; icon_id icon; const char *label; } option;

static int build_options(const vam_item *it, option *o)
{
    int n = 0;
    bool track = it->kind == VAM_KIND_TRACK || it->kind == VAM_KIND_EPISODE;
    if (track) {
        o[n++] = (option){ ACT_SAVE, it->is_saved ? ICON_HEART : ICON_HEART_OUTLINE,
                           it->is_saved ? "Remove from Liked Songs" : "Add to Liked Songs" };
        o[n++] = (option){ ACT_QUEUE, ICON_QUEUE, "Add to queue" };
        if (it->album_uri[0])
            o[n++] = (option){ ACT_ALBUM, ICON_DISC, "Go to album" };
        if (it->artist_uri[0])
            o[n++] = (option){ ACT_ARTIST, ICON_PERSON, "Go to artist" };
        return n;
    }
    o[n++] = (option){ ACT_PLAY, ICON_PLAY, "Play" };
    if (it->kind == VAM_KIND_ARTIST)
        o[n++] = (option){ ACT_SAVE, ICON_PERSON, it->is_saved ? "Unfollow" : "Follow" };
    else if (it->kind != VAM_KIND_COLLECTION)
        o[n++] = (option){ ACT_SAVE, it->is_saved ? ICON_HEART : ICON_HEART_OUTLINE,
                           it->is_saved ? "Remove from Your Library" : "Save to Your Library" };
    if (it->artist_uri[0])
        o[n++] = (option){ ACT_ARTIST, ICON_PERSON, "Go to artist" };
    return n;
}

static void run(act a, const vam_item *it)
{
    char uri[VAM_URI_MAX];
    switch (a) {
    case ACT_PLAY:
        app_play_item(it, NULL, 0);
        app_close_sheet();
        break;
    case ACT_SAVE:
        app_toggle_saved(it->uri, it->is_saved);
        app_close_sheet();
        break;
    case ACT_QUEUE:
        if (vam_add_to_queue(it->uri) == VAM_OK)
            app_toast("Added to queue");
        else
            app_toast("Couldn't add to queue.");
        app_close_sheet();
        break;
    case ACT_ALBUM:
        snprintf(uri, sizeof(uri), "%s", it->album_uri);
        app_open_uri(uri);
        break;
    case ACT_ARTIST:
        snprintf(uri, sizeof(uri), "%s", it->artist_uri);
        app_open_uri(uri);
        break;
    }
}

void screen_sheet(void)
{
    sheet *s = &A.sheet;
    float t = s->anim;
    float e = 1 - (1 - t) * (1 - t);
    gfx_rect(rect(0, 0, SCREEN_W, SCREEN_H), rgba(0x000000, (uint8_t)(160 * e)));
    if (ui_item(ui_id("sheet.dismiss"), rect(0, 0, SCREEN_W, SCREEN_H), UI_F_NOFOCUS).clicked)
        app_close_sheet();

    option opts[6];
    int n = build_options(&s->item, opts);
    const float row_h = 50, head_h = 80, w = 520;
    float h = head_h + n * row_h + 16;
    float x = (SCREEN_W - w) / 2, y = SCREEN_H - h * e;
    rectf panel = rect(x, y, w, h + 20);
    ui_item(ui_id("sheet.panel"), panel, UI_F_NOFOCUS);  /* taps on the panel don't dismiss */
    gfx_rrect(panel, 14, rgb(C_SURFACE_HI));

    float rad = s->item.kind == VAM_KIND_ARTIST ? 26 : 4;
    draw_cover(s->item.image_url, rect(x + 18, y + 14, 52, 52), rad, s->item.kind, s->item.title);
    text_draw(s->item.title, x + 84, y + 18, FS_M, FW_BOLD, rgb(C_TEXT), w - 100);
    text_draw(s->item.subtitle, x + 84, y + 40, FS_S, FW_REGULAR, rgb(C_SUBTEXT), w - 100);
    gfx_rect(rect(x, y + head_h - 1, w, 1), rgb(C_DIVIDER));

    for (int i = 0; i < n; i++) {
        rectf r = rect(x + 8, y + head_h + 6 + i * row_h, w - 16, row_h);
        ui_res res = ui_item(ui_idx(ui_id("sheet.opt"), i), r, 0);
        if (res.pressed || res.focused)
            gfx_rrect(r, 6, rgb(C_SURFACE_HI2));
        bool green = opts[i].a == ACT_SAVE && s->item.is_saved;
        gfx_icon(opts[i].icon, r.x + 26, r.y + r.h / 2, 22, green ? rgb(C_ACCENT) : rgb(C_SUBTEXT));
        text_draw_vc(opts[i].label, r.x + 56, r.y + r.h / 2, FS_M, FW_REGULAR, rgb(C_TEXT), r.w - 70);
        if (res.focused)
            draw_focus_ring(r, 6);
        if (res.clicked && s->open)
            run(opts[i].a, &s->item);
    }
}
