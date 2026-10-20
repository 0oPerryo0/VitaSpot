/* Shown until the backend reports a logged-in Premium account. */

#include <stdio.h>

#include "../app.h"
#include "../gfx/text.h"
#include "../theme.h"

static void brand(float cy)
{
    gfx_rrect(rect(SCREEN_W / 2.0f - 32, cy - 32, 64, 64), 18, rgb(C_ACCENT));
    gfx_icon(ICON_NOTE, SCREEN_W / 2.0f, cy, 40, rgb(0x000000));
}

static void footer(void)
{
    text_draw_aligned("Unofficial client. Not affiliated with or endorsed by Spotify. Requires Spotify Premium.",
                      SCREEN_W / 2.0f, SCREEN_H - 30, FS_XS, FW_REGULAR, rgb(C_MUTED), SCREEN_W - 40, ALIGN_CENTER);
}

static void step(int n, const char *text, float x, float y, float w)
{
    char num[4];
    snprintf(num, sizeof(num), "%d", n);
    gfx_circle(x + 16, y + 14, 16, rgb(C_SURFACE_HI2));
    text_draw_aligned(num, x + 16, y + 14 - text_line_height(FS_S) / 2.0f, FS_S, FW_BOLD, rgb(C_TEXT), 0,
                      ALIGN_CENTER);
    text_draw_vc(text, x + 46, y + 14, FS_M, FW_REGULAR, rgb(C_TEXT), w - 46);
}

void screen_login(void)
{
    ui_set_enabled(true);
    gfx_vgradient(rect(0, 0, SCREEN_W, SCREEN_H), rgb(0x1B3A2A), rgb(C_BG));
    brand(84);
    text_draw_aligned("VitaSpot", SCREEN_W / 2.0f, 126, FS_XXL, FW_BOLD, rgb(C_TEXT), 0, ALIGN_CENTER);

    const vam_auth_state *a = &A.auth;
    const char *status;
    color sc = rgb(C_SUBTEXT);
    bool busy = false;
    switch (a->status) {
    case VAM_AUTH_STARTING: status = "Starting up..."; busy = true; break;
    case VAM_AUTH_CONNECTING: status = "Signing in..."; busy = true; break;
    case VAM_AUTH_ERROR: status = "Couldn't sign in"; sc = rgb(C_ERROR); break;
    default: status = "Connect from the Spotify app"; break;
    }
    text_draw_aligned(status, SCREEN_W / 2.0f, 172, FS_L, FW_BOLD, sc, 0, ALIGN_CENTER);
    if (a->message[0])
        text_draw_aligned(a->message, SCREEN_W / 2.0f, 202, FS_S, FW_REGULAR, rgb(C_SUBTEXT), 700, ALIGN_CENTER);

    rectf card = rect(160, 236, 640, 176);
    gfx_rrect(card, 12, rgba(0x000000, 110));
    char pick[300];
    snprintf(pick, sizeof(pick), "Tap the devices icon and choose \"%s\".",
             a->device_name[0] ? a->device_name : "PS Vita");
    step(1, "Connect this Vita and your phone to the same Wi-Fi.", card.x + 24, card.y + 22, card.w - 48);
    step(2, "Open Spotify on your phone or computer.", card.x + 24, card.y + 72, card.w - 48);
    step(3, pick, card.x + 24, card.y + 122, card.w - 48);
    if (busy)
        gfx_spinner(card.x + card.w - 30, card.y + 30, 10, A.time, rgb(C_ACCENT));

    if (a->login_url[0]) {
        char alt[400];
        if (a->login_code[0])
            snprintf(alt, sizeof(alt), "Or open %s and enter %s", a->login_url, a->login_code);
        else
            snprintf(alt, sizeof(alt), "Or open %s", a->login_url);
        text_draw_aligned(alt, SCREEN_W / 2.0f, 428, FS_S, FW_REGULAR, rgb(C_TEXT), 760, ALIGN_CENTER);
    }
    footer();
}

void screen_premium(void)
{
    ui_set_enabled(true);
    gfx_vgradient(rect(0, 0, SCREEN_W, SCREEN_H), rgb(0x3A1B24), rgb(C_BG));
    brand(110);
    text_draw_aligned("Spotify Premium required", SCREEN_W / 2.0f, 168, FS_XXL, FW_BOLD, rgb(C_TEXT), 0,
                      ALIGN_CENTER);
    text_draw_aligned("VitaSpot streams through Spotify Connect, which only works with Premium accounts.",
                      SCREEN_W / 2.0f, 220, FS_M, FW_REGULAR, rgb(C_SUBTEXT), 800, ALIGN_CENTER);
    char who[300];
    snprintf(who, sizeof(who), "Signed in as %s",
             A.auth.display_name[0] ? A.auth.display_name : A.auth.username);
    text_draw_aligned(who, SCREEN_W / 2.0f, 252, FS_M, FW_REGULAR, rgb(C_SUBTEXT), 800, ALIGN_CENTER);
    float w = text_width("Log out", FS_S, FW_REGULAR, 0) + 32.0f;
    if (pill_button(ui_id("premium.logout"), "Log out", (SCREEN_W - w) / 2, 300, false, NULL))
        vam_logout();
    footer();
}
