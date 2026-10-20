/* Settings tab: the signed-in account (with log out), screen power, audio quality, and about. */

#include <stdio.h>

#include "../app.h"
#include "../gfx/images.h"
#include "../gfx/text.h"
#include "../power.h"
#include "../prefs.h"
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

#define SCREEN_ROW_H 64

static void fmt_seconds(int s, char *buf, size_t n)
{
    if (s <= 0)
        snprintf(buf, n, "Never");
    else if (s < 60)
        snprintf(buf, n, "%d seconds", s);
    else if (s < 120)
        snprintf(buf, n, "1 minute");
    else
        snprintf(buf, n, "%d minutes", s / 60);
}

/* The choice after cur, wrapping; tapping the value cycles through them. */
static int next_choice(const int *choices, int cur)
{
    for (int i = 0; i < POWER_CHOICE_COUNT; i++)
        if (choices[i] == cur)
            return choices[(i + 1) % POWER_CHOICE_COUNT];
    return choices[0];
}

static void timer_row(uint32_t id, const char *label, int *value, const int *choices, float x, float cy, float right)
{
    text_draw_vc(label, x, cy, FS_M, FW_REGULAR, rgb(C_TEXT), right - x - 160);
    char val[32];
    fmt_seconds(*value, val, sizeof(val));
    if (pill_button(id, val, right - pill_w(val), cy - 20, false, NULL)) {
        *value = next_choice(choices, *value);
        prefs_changed();
    }
}

static float screen_card(float x, float y, float w)
{
    prefs *ps = prefs_get();
    rectf card = rect(x, y, w, SCREEN_ROW_H * 3);
    gfx_rrect(card, 8, rgb(C_SURFACE));
    for (int i = 1; i < 3; i++)
        gfx_rect(rect(card.x + 16, card.y + SCREEN_ROW_H * i, card.w - 32, 1), rgb(C_DIVIDER));
    float lx = card.x + 16, right = card.x + card.w - 16;

    /* Screen-off combo: "Change", then hold the two new buttons together. */
    btn a, b;
    if (ui_captured_combo(&a, &b)) {
        ps->combo[0] = a;
        ps->combo[1] = b;
        prefs_changed();
        power_apply_prefs();
    }
    bool capturing = ui_capturing_combo();
    float cy = card.y + SCREEN_ROW_H / 2.0f;
    text_draw_vc("Screen off combo", lx, cy, FS_M, FW_REGULAR, rgb(C_TEXT), 260);
    const char *action = capturing ? "Cancel" : "Change";
    float aw = pill_w(action);
    char combo[64];
    snprintf(combo, sizeof(combo), "%s + %s", platform_btn_label(ps->combo[0]), platform_btn_label(ps->combo[1]));
    text_draw_aligned(capturing ? "Hold two buttons..." : combo, right - aw - 16, cy - text_line_height(FS_M) / 2.0f,
                      FS_M, capturing ? FW_BOLD : FW_REGULAR, rgb(capturing ? C_ACCENT : C_SUBTEXT), 0, ALIGN_RIGHT);
    if (pill_button(ui_id("settings.combo"), action, right - aw, cy - 20, false, NULL))
        ui_capture_combo(!capturing);

    timer_row(ui_id("settings.dim"), "Dim screen after", &ps->dim_after, POWER_DIM_CHOICES, lx,
              card.y + SCREEN_ROW_H * 1.5f, right);
    timer_row(ui_id("settings.off"), "Turn screen off after", &ps->off_after, POWER_OFF_CHOICES, lx,
              card.y + SCREEN_ROW_H * 2.5f, right);

    text_draw("Music keeps playing with the screen off. Press any button to turn it back on.", x,
              card.y + card.h + 10, FS_S, FW_REGULAR, rgb(C_MUTED), w);
    return card.h + 10 + text_line_height(FS_S);
}

static const int BITRATES[] = { 320, 160, 96 };

static const char *quality_label(int kbps)
{
    return kbps == 320 ? "High (320 kbps)" : kbps == 160 ? "Normal (160 kbps)" : "Low (96 kbps)";
}

static float audio_card(float x, float y, float w)
{
    prefs *p = prefs_get();
    rectf card = rect(x, y, w, SCREEN_ROW_H);
    gfx_rrect(card, 8, rgb(C_SURFACE));
    float cy = card.y + SCREEN_ROW_H / 2.0f, right = card.x + card.w - 16;
    text_draw_vc("Audio quality", card.x + 16, cy, FS_M, FW_REGULAR, rgb(C_TEXT), 260);
    const char *val = quality_label(p->bitrate);
    if (pill_button(ui_id("settings.quality"), val, right - pill_w(val), cy - 20, false, NULL)) {
        int i = 0;
        while (i < 3 && BITRATES[i] != p->bitrate)
            i++;
        int next = BITRATES[(i + 1) % 3];
        /* Keep the shown and saved value in step with what actually plays. */
        if (vam_set_bitrate(next) == VAM_OK) {
            p->bitrate = next;
            prefs_changed();
        } else {
            app_toast("Couldn't change the audio quality.");
        }
    }
    text_draw("Applies from the next song. Lower quality uses less data and starts songs faster.", x,
              card.y + card.h + 10, FS_S, FW_REGULAR, rgb(C_MUTED), w);
    return card.h + 10 + text_line_height(FS_S);
}

static float about_card(float x, float y, float w)
{
    rectf card = rect(x, y, w, 96);
    gfx_rrect(card, 8, rgb(C_SURFACE));
    text_draw("VitaSpot", card.x + 16, card.y + 20, FS_M, FW_BOLD, rgb(C_TEXT), card.w - 32);
    text_draw_aligned("Version " VAM_APP_VERSION, card.x + card.w - 16, card.y + 22, FS_S, FW_REGULAR,
                      rgb(C_SUBTEXT), 0, ALIGN_RIGHT);
    text_draw("An unofficial Spotify client for the PS Vita. Not affiliated with Spotify.", card.x + 16,
              card.y + 50, FS_S, FW_REGULAR, rgb(C_SUBTEXT), card.w - 32);
    return card.h;
}

void screen_settings(view *v)
{
    /* Coming back to the tab starts from the plain Log out button. */
    if (A.time - g_last_drawn > 0.25f) {
        g_confirm_logout = false;
        if (ui_capturing_combo())
            ui_capture_combo(false);
    }
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

    text_draw("Screen", x, y, FS_L, FW_BOLD, rgb(C_TEXT), w);
    y += 38;
    y += screen_card(x, y, w) + 32;

    text_draw("Audio", x, y, FS_L, FW_BOLD, rgb(C_TEXT), w);
    y += 38;
    y += audio_card(x, y, w) + 32;

    text_draw("About", x, y, FS_L, FW_BOLD, rgb(C_TEXT), w);
    y += 38;
    y += about_card(x, y, w);

    v->content_h = y + v->scroll.pos - CONTENT_Y + 24;
    ui_scroll_end();
}
