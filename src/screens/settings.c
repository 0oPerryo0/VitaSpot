/* Settings tab: the signed-in account (with log out) and about. */

#include <stdio.h>

#include "../app.h"
#include "../gfx/images.h"
#include "../gfx/text.h"
#include "../theme.h"

/* Log out asks once more before it forgets the saved login. */
static bool g_confirm_logout;
static float g_last_drawn;

static float pill_w(const char *label) { return text_width(label, FS_S, FW_REGULAR, 0) + 32.0f; }

static float account_card(float x, float y, float w)
{
    rectf card = rect(x, y, w, 96);
    gfx_rrect(card, 8, rgb(C_SURFACE));

    rectf pic = rect(card.x + 16, card.y + 16, 64, 64);
    SDL_Texture *avatar = images_get_round(A.auth.image_url, NULL);
    if (avatar) {
        gfx_image(avatar, pic, 255);
    } else {
        gfx_circle(pic.x + 32, pic.y + 32, 32, rgb(C_SURFACE_HI2));
        gfx_icon(ICON_PERSON, pic.x + 32, pic.y + 32, 32, rgb(C_SUBTEXT));
    }

    /* Buttons sit at the right; the text gets what is left. */
    const char *confirm = "Log out", *cancel = "Cancel";
    float by = card.y + (card.h - 40) / 2, right = card.x + card.w - 16;
    float buttons_w = g_confirm_logout ? pill_w(cancel) + 8 + pill_w(confirm) : pill_w(confirm);
    float tx = pic.x + pic.w + 16, tw = right - buttons_w - 16 - tx;

    const char *name = A.auth.display_name[0] ? A.auth.display_name : A.auth.username;
    text_draw(name, tx, card.y + 22, FS_L, FW_BOLD, rgb(C_TEXT), tw);
    if (g_confirm_logout)
        text_draw("Log out of Spotify on this Vita?", tx, card.y + 54, FS_S, FW_REGULAR, rgb(C_TEXT), tw);
    else
        text_draw(A.auth.is_premium ? "Spotify Premium" : "Spotify Free", tx, card.y + 54, FS_S, FW_REGULAR,
                  rgb(C_SUBTEXT), tw);

    if (g_confirm_logout) {
        float cx = right - buttons_w;
        if (pill_button(ui_id("settings.cancel"), cancel, cx, by, false, NULL)) {
            g_confirm_logout = false;
            ui_set_focus(ui_id("settings.logout"));
        }
        if (pill_button(ui_id("settings.confirm"), confirm, cx + pill_w(cancel) + 8, by, true, NULL)) {
            g_confirm_logout = false;
            if (vam_logout() != VAM_OK)
                app_toast("Couldn't log out. Try again.");
        }
    } else if (pill_button(ui_id("settings.logout"), confirm, right - buttons_w, by, false, NULL)) {
        g_confirm_logout = true;
        ui_set_focus(ui_id("settings.cancel"));
    }
    return card.h;
}

static float about_card(float x, float y, float w)
{
    rectf card = rect(x, y, w, 96);
    gfx_rrect(card, 8, rgb(C_SURFACE));
    text_draw("VitaSpot", card.x + 16, card.y + 20, FS_M, FW_BOLD, rgb(C_TEXT), card.w - 32);
    text_draw("An unofficial Spotify client for the PS Vita. Not affiliated with Spotify.", card.x + 16,
              card.y + 50, FS_S, FW_REGULAR, rgb(C_SUBTEXT), card.w - 32);
    return card.h;
}

void screen_settings(view *v)
{
    /* Coming back to the tab starts from the plain Log out button. */
    if (A.time - g_last_drawn > 0.25f)
        g_confirm_logout = false;
    g_last_drawn = A.time;

    rectf vp = rect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H);
    gfx_vgradient(rect(CONTENT_X, 0, CONTENT_W, 280), rgb(0x2A2A2A), rgb(C_BG));
    ui_scroll_begin(&v->scroll, vp, v->content_h, AXIS_Y, true);
    float x = CONTENT_X + PAD, w = CONTENT_W - 2 * PAD;
    float y = CONTENT_Y + 22 - v->scroll.pos;
    text_draw("Settings", x, y, FS_XXL, FW_BOLD, rgb(C_TEXT), w);
    y += 68;

    text_draw("Account", x, y, FS_L, FW_BOLD, rgb(C_TEXT), w);
    y += 38;
    y += account_card(x, y, w) + 32;

    text_draw("About", x, y, FS_L, FW_BOLD, rgb(C_TEXT), w);
    y += 38;
    y += about_card(x, y, w);

    v->content_h = y + v->scroll.pos - CONTENT_Y + 24;
    ui_scroll_end();
}
