#include "app.h"

#include <stdio.h>
#include <string.h>

#include "gfx/images.h"
#include "gfx/text.h"
#include "platform/platform.h"
#include "theme.h"

app A;

#define PENDING_SAVES 16
static struct { uint32_t req; char uri[VAM_URI_MAX]; bool prev; } g_saves[PENDING_SAVES];
static uint32_t g_focus_before_overlay;
static float g_edit_changed_at = -1;

/* ------------------------------------------------------------------------ */
/* Views                                                                     */
/* ------------------------------------------------------------------------ */

static void view_init(view *v, view_type t, const char *uri)
{
    memset(v, 0, sizeof(*v));
    v->type = t;
    snprintf(v->uri, sizeof(v->uri), "%s", uri ? uri : "");
}

static void view_free(view *v)
{
    if (v->req) vam_cancel(v->req);
    if (v->more_req) vam_cancel(v->more_req);
    model_free(v->page);
    memset(v, 0, sizeof(*v));
}

static void reset_stacks(void)
{
    for (int t = 0; t < TAB_COUNT; t++) {
        for (int d = 0; d < A.depth[t]; d++)
            view_free(&A.stacks[t][d]);
        A.depth[t] = 1;
    }
    view_init(&A.stacks[TAB_HOME][0], VIEW_HOME, "vam:home");
    view_init(&A.stacks[TAB_SEARCH][0], VIEW_SEARCH, "");
    view_init(&A.stacks[TAB_LIBRARY][0], VIEW_LIBRARY, "vam:library");
    A.tab = TAB_HOME;
    A.np_open = false;
    A.sheet.open = false;
    ui_set_focus(0);
    ui_reset_focus_list();
}

view *app_view(void) { return &A.stacks[A.tab][A.depth[A.tab] - 1]; }
int app_depth(void) { return A.depth[A.tab]; }

void app_request_view(view *v)
{
    char uri[VAM_URI_MAX + 256];
    if (v->type == VIEW_SEARCH) {
        if (!v->query[0])
            return;
        snprintf(uri, sizeof(uri), "vam:search:%s", v->query);
    } else {
        snprintf(uri, sizeof(uri), "%s", v->uri);
    }
    if (v->req)
        vam_cancel(v->req);
    if (v->more_req) {
        vam_cancel(v->more_req);
        v->more_req = 0;
    }
    v->failed = false;
    v->req = vam_request_page(uri, 0, 50);
    if (!v->req) {
        v->failed = true;
        snprintf(v->error, sizeof(v->error), "Couldn't reach the player service.");
    }
}

void app_request_more(view *v)
{
    if (v->more_req || v->req || !model_has_more(v->page))
        return;
    v->more_req = vam_request_page(v->page->uri, v->page->sections[0].count, 50);
}

static view *find_view_by_req(uint32_t id, bool *is_more)
{
    for (int t = 0; t < TAB_COUNT; t++) {
        for (int d = 0; d < A.depth[t]; d++) {
            view *v = &A.stacks[t][d];
            if (v->req == id) { *is_more = false; return v; }
            if (v->more_req == id) { *is_more = true; return v; }
        }
    }
    return NULL;
}

static void apply_saved(const char *uri, bool saved)
{
    for (int t = 0; t < TAB_COUNT; t++)
        for (int d = 0; d < A.depth[t]; d++)
            model_set_saved(A.stacks[t][d].page, uri, saved);
    if (A.player.has_track && strcmp(A.player.track.uri, uri) == 0)
        A.player.track.is_saved = saved;
    if (strcmp(A.sheet.item.uri, uri) == 0)
        A.sheet.item.is_saved = saved;
}

/* ------------------------------------------------------------------------ */
/* Navigation (deferred to end of frame)                                     */
/* ------------------------------------------------------------------------ */

void app_open_uri(const char *uri)
{
    if (!uri || !*uri)
        return;
    A.nav = NAV_PUSH;
    snprintf(A.nav_uri, sizeof(A.nav_uri), "%s", uri);
}

void app_go_back(void) { A.nav = NAV_POP; }

void app_switch_tab(int tab)
{
    A.nav = NAV_TAB;
    A.nav_tab = tab;
}

static void close_overlays(void)
{
    A.np_open = false;
    A.sheet.open = false;
}

static void apply_nav(void)
{
    nav_kind k = A.nav;
    A.nav = NAV_NONE;
    if (k == NAV_NONE)
        return;
    view *cur = app_view();
    cur->saved_focus = ui_focus();
    int *depth = &A.depth[A.tab];

    if (k == NAV_PUSH) {
        close_overlays();
        if (strcmp(cur->uri, A.nav_uri) == 0 && cur->type == VIEW_PAGE)
            return;
        if (*depth >= MAX_DEPTH) {
            /* Drop the oldest non-root view to make room. */
            view_free(&A.stacks[A.tab][1]);
            memmove(&A.stacks[A.tab][1], &A.stacks[A.tab][2], sizeof(view) * (MAX_DEPTH - 2));
            memset(&A.stacks[A.tab][MAX_DEPTH - 1], 0, sizeof(view));
            (*depth)--;
        }
        view *v = &A.stacks[A.tab][(*depth)++];
        view_init(v, VIEW_PAGE, A.nav_uri);
        ui_set_focus(0);
    } else if (k == NAV_POP) {
        if (*depth <= 1)
            return;
        view_free(cur);
        (*depth)--;
        ui_set_focus(app_view()->saved_focus);
    } else if (k == NAV_TAB) {
        close_overlays();
        if (A.nav_tab == A.tab) {
            /* Tapping the active tab returns to its root. */
            while (*depth > 1)
                view_free(&A.stacks[A.tab][--(*depth)]);
            ui_scroll_to(&app_view()->scroll, 0);
            if (A.tab == TAB_SEARCH)
                app_begin_typing();
        } else {
            A.tab = A.nav_tab;
        }
        ui_set_focus(app_view()->saved_focus);
    }
    ui_reset_focus_list();
}

/* ------------------------------------------------------------------------ */
/* Player helpers                                                            */
/* ------------------------------------------------------------------------ */

bool app_is_playing(void)
{
    return A.player.has_track &&
           (A.player.playback == VAM_PLAYBACK_PLAYING || A.player.playback == VAM_PLAYBACK_BUFFERING);
}

uint32_t app_position_ms(void)
{
    if (!A.player.has_track)
        return 0;
    uint64_t pos = A.player.position_ms;
    if (A.player.playback == VAM_PLAYBACK_PLAYING) {
        uint64_t now = vam_now_ms();
        if (now > A.player.position_timestamp_ms)
            pos += now - A.player.position_timestamp_ms;
    }
    if (A.player.track.duration_ms && pos > A.player.track.duration_ms)
        pos = A.player.track.duration_ms;
    return (uint32_t)pos;
}

bool app_is_current_track(const char *uri)
{
    return A.player.has_track && uri && *uri && strcmp(A.player.track.uri, uri) == 0;
}

bool app_is_current_context(const char *uri)
{
    return A.player.has_track && uri && *uri && strcmp(A.player.context_uri, uri) == 0;
}

void app_toast(const char *msg)
{
    if (!msg || !*msg)
        return;
    snprintf(A.toast, sizeof(A.toast), "%s", msg);
    A.toast_until = A.time + 2.6f;
}

static void check(vam_result r, const char *what)
{
    if (r == VAM_OK)
        return;
    char buf[160];
    if (r == VAM_ERR_NOT_PREMIUM)
        snprintf(buf, sizeof(buf), "Spotify Premium is required to %s.", what);
    else if (r == VAM_ERR_NETWORK)
        snprintf(buf, sizeof(buf), "No connection. Couldn't %s.", what);
    else
        snprintf(buf, sizeof(buf), "Couldn't %s.", what);
    app_toast(buf);
}

void app_play_item(const vam_item *it, const char *context_uri, uint32_t index)
{
    if (!it)
        return;
    if (!it->is_playable && (it->kind == VAM_KIND_TRACK || it->kind == VAM_KIND_EPISODE)) {
        app_toast("This song isn't available.");
        return;
    }
    switch (it->kind) {
    case VAM_KIND_TRACK:
    case VAM_KIND_EPISODE:
        check(vam_play(context_uri, it->uri, index), "play this");
        break;
    default:
        check(vam_play(it->uri, NULL, 0), "play this");
        break;
    }
}

void app_toggle_saved(const char *uri, bool currently_saved)
{
    uint32_t req = vam_set_saved(uri, !currently_saved);
    if (!req) {
        app_toast("Couldn't update your library.");
        return;
    }
    for (int i = 0; i < PENDING_SAVES; i++) {
        if (!g_saves[i].req) {
            g_saves[i].req = req;
            g_saves[i].prev = currently_saved;
            snprintf(g_saves[i].uri, sizeof(g_saves[i].uri), "%s", uri);
            break;
        }
    }
    /* Optimistic: flip now, revert if the request fails. */
    apply_saved(uri, !currently_saved);
    bool is_track = strncmp(uri, "spotify:track:", 14) == 0;
    if (is_track)
        app_toast(currently_saved ? "Removed from Liked Songs" : "Added to Liked Songs");
    else
        app_toast(currently_saved ? "Removed from Your Library" : "Saved to Your Library");
}

void app_open_sheet(const vam_item *item, const char *context_uri, uint32_t index)
{
    if (!A.sheet.open && !A.np_open)
        g_focus_before_overlay = ui_focus();
    A.sheet.open = true;
    A.sheet.item = *item;
    snprintf(A.sheet.context_uri, sizeof(A.sheet.context_uri), "%s", context_uri ? context_uri : "");
    A.sheet.index = index;
    ui_set_focus(0);
    ui_reset_focus_list();
}

void app_close_sheet(void)
{
    A.sheet.open = false;
    ui_set_focus(A.np_open ? 0 : g_focus_before_overlay);
    ui_reset_focus_list();
}

void app_set_now_playing(bool open)
{
    if (open == A.np_open)
        return;
    if (open && !A.player.has_track)
        return;
    if (open)
        g_focus_before_overlay = ui_focus();
    A.np_open = open;
    ui_set_focus(open ? 0 : g_focus_before_overlay);
    ui_reset_focus_list();
}

/* ------------------------------------------------------------------------ */
/* Search text entry                                                         */
/* ------------------------------------------------------------------------ */

static void load_recent(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%srecent_searches.txt", platform_data_dir());
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    int n = 0;
    while (n < RECENT_SEARCHES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            snprintf(A.recent[n++], sizeof(A.recent[0]), "%s", line);
    }
    fclose(f);
}

static void save_recent(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%srecent_searches.txt", platform_data_dir());
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < RECENT_SEARCHES; i++)
        if (A.recent[i][0])
            fprintf(f, "%s\n", A.recent[i]);
    fclose(f);
}

static void add_recent(const char *q)
{
    int found = RECENT_SEARCHES - 1;
    for (int i = 0; i < RECENT_SEARCHES; i++) {
        if (strcmp(A.recent[i], q) == 0) {
            found = i;
            break;
        }
    }
    for (int i = found; i > 0; i--)
        memcpy(A.recent[i], A.recent[i - 1], sizeof(A.recent[0]));
    snprintf(A.recent[0], sizeof(A.recent[0]), "%s", q);
    save_recent();
}

static void end_typing(bool commit);

static void run_search(const char *q, bool remember)
{
    view *root = &A.stacks[TAB_SEARCH][0];
    while (A.depth[TAB_SEARCH] > 1)
        view_free(&A.stacks[TAB_SEARCH][--A.depth[TAB_SEARCH]]);
    if (strcmp(root->query, q) == 0 && root->page)
        return;
    snprintf(root->query, sizeof(root->query), "%s", q);
    model_free(root->page);
    root->page = NULL;
    root->scroll.pos = 0;
    for (int i = 0; i < MAX_SHELVES; i++)
        root->shelves[i].pos = 0;
    if (q[0]) {
        app_request_view(root);
        if (remember)
            add_recent(q);
    }
}

void app_set_search(const char *query)
{
    if (A.typing)
        end_typing(false);
    run_search(query, true);
}

void app_begin_typing(void)
{
    A.typing = true;
    snprintf(A.edit, sizeof(A.edit), "%s", A.stacks[TAB_SEARCH][0].query);
    g_edit_changed_at = -1;
    SDL_StartTextInput();
    /* The Vita IME dialog is modal, so navigation needs no text mode there. */
    if (!platform_is_vita())
        ui_set_text_mode(true);
}

static void end_typing(bool commit)
{
    A.typing = false;
    SDL_StopTextInput();
    ui_set_text_mode(false);
    if (commit) {
        /* Trim surrounding spaces. */
        char *s = A.edit;
        while (*s == ' ') s++;
        size_t n = strlen(s);
        while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
        run_search(s, true);
    }
}

static void utf8_pop(char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        n--;
        if (((unsigned char)s[n] & 0xC0) != 0x80)
            break;
    }
    s[n] = 0;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

void app_handle_sdl_event(const SDL_Event *e)
{
    if (e->type == SDL_QUIT) {
        A.quit = true;
        return;
    }
    if (A.typing && e->type == SDL_TEXTINPUT) {
        if (platform_is_vita()) {
            /* The IME returns the whole edited string at once. */
            snprintf(A.edit, sizeof(A.edit), "%s", e->text.text);
            end_typing(true);
        } else if (strlen(A.edit) + strlen(e->text.text) < sizeof(A.edit) - 1) {
            strcat(A.edit, e->text.text);
            g_edit_changed_at = A.time;
        }
        return;
    }
    if (A.typing && !platform_is_vita() && e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.sym) {
        case SDLK_BACKSPACE:
            utf8_pop(A.edit);
            g_edit_changed_at = A.time;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            end_typing(true);
            break;
        case SDLK_ESCAPE:
            end_typing(false);
            break;
        default:
            break;
        }
        return;
    }
    ui_handle_event(e);
}

static void handle_vam_event(vam_event *ev)
{
    switch (ev->type) {
    case VAM_EV_AUTH_CHANGED: {
        int before = A.auth.status;
        vam_get_auth_state(&A.auth);
        if (before == VAM_AUTH_LOGGED_IN && A.auth.status != VAM_AUTH_LOGGED_IN)
            reset_stacks();
        break;
    }
    case VAM_EV_PLAYER_CHANGED:
        vam_get_player_state(&A.player);
        A.have_player = true;
        break;
    case VAM_EV_PAGE: {
        bool more = false;
        view *v = find_view_by_req(ev->request_id, &more);
        if (!v)
            break;
        if (more) {
            v->more_req = 0;
            if (ev->result == VAM_OK && ev->page)
                model_append(v->page, ev->page);
            break;
        }
        v->req = 0;
        if (ev->result == VAM_OK && ev->page) {
            model_free(v->page);
            v->page = model_from_vam(ev->page);
            v->failed = v->page == NULL;
        } else if (!v->page) {
            v->failed = true;
            snprintf(v->error, sizeof(v->error), "%s",
                     ev->message[0] ? ev->message : "Something went wrong.");
        }
        break;
    }
    case VAM_EV_IMAGE:
        images_on_event(ev);
        break;
    case VAM_EV_REQUEST_DONE:
        for (int i = 0; i < PENDING_SAVES; i++) {
            if (g_saves[i].req != ev->request_id)
                continue;
            if (ev->result != VAM_OK) {
                apply_saved(g_saves[i].uri, g_saves[i].prev);
                app_toast(ev->message[0] ? ev->message : "Couldn't update your library.");
            }
            g_saves[i].req = 0;
        }
        break;
    case VAM_EV_SAVED_CHANGED:
        apply_saved(ev->uri, ev->saved != 0);
        /* Library contents changed: refresh it in the background. */
        if (A.stacks[TAB_LIBRARY][0].page)
            app_request_view(&A.stacks[TAB_LIBRARY][0]);
        break;
    case VAM_EV_ERROR:
    case VAM_EV_TOAST:
        app_toast(ev->message);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

bool app_init(SDL_Window *w, SDL_Renderer *r)
{
    memset(&A, 0, sizeof(A));
    A.window = w;
    A.renderer = r;
    A.np_color = rgb(0x404040);
    reset_stacks();
    load_recent();

    vam_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.api_version = VAM_API_VERSION;
    cfg.device_name = platform_is_vita() ? "PS Vita" : "VitaSpot Desktop";
    cfg.data_dir = platform_data_dir();
    cfg.bitrate = platform_is_vita() ? 320 : 160;
    /* The Vita's speaker is weak: start at full volume and skip loudness
       normalisation, which turns most tracks down by several dB. */
    cfg.initial_volume = platform_is_vita() ? 65535 : 52000;
    cfg.normalize_volume = platform_is_vita() ? 0 : 1;
    vam_result res = vam_init(&cfg);
    if (res != VAM_OK)
        SDL_Log("vam_init failed: %d", res);
    vam_get_auth_state(&A.auth);
    vam_get_player_state(&A.player);
    return true;
}

void app_shutdown(void)
{
    for (int t = 0; t < TAB_COUNT; t++)
        for (int d = 0; d < A.depth[t]; d++)
            view_free(&A.stacks[t][d]);
    vam_shutdown();
}

static void global_buttons(void)
{
    if (ui_btn(BTN_BACK)) {
        ui_consume(BTN_BACK);
        if (A.typing)
            end_typing(false);
        else if (A.sheet.open)
            app_close_sheet();
        else if (A.np_open)
            app_set_now_playing(false);
        else if (app_depth() > 1)
            app_go_back();
    }
    if (ui_btn(BTN_START)) {
        ui_consume(BTN_START);
        app_set_now_playing(!A.np_open);
    }
    if (ui_btn(BTN_TRIANGLE)) {
        ui_consume(BTN_TRIANGLE);
        if (A.player.has_track)
            check(app_is_playing() ? vam_pause() : vam_resume(), "change playback");
    }
    if (ui_btn(BTN_L)) {
        ui_consume(BTN_L);
        if (A.player.has_track)
            check(vam_prev(), "skip back");
    }
    if (ui_btn(BTN_R)) {
        ui_consume(BTN_R);
        if (A.player.has_track)
            check(vam_next(), "skip");
    }
    if (ui_btn(BTN_SELECT)) {
        ui_consume(BTN_SELECT);
        app_open_uri("vam:queue");
    }
}

static void draw_toast(void)
{
    if (!A.toast[0] || A.time > A.toast_until)
        return;
    float left = A.toast_until - A.time;
    float a = left < 0.3f ? left / 0.3f : 1;
    int tw = text_width(A.toast, FS_M, FW_BOLD, 600);
    float w = tw + 40.0f, h = 44;
    float x = (SCREEN_W - w) / 2, y = SCREEN_H - PLAYER_H - h - 16;
    gfx_shadow(rect(x, y, w, h), 8, 10, (uint8_t)(120 * a));
    gfx_rrect(rect(x, y, w, h), 8, with_alpha(rgb(0xFFFFFF), a));
    text_draw_vc(A.toast, x + 20, y + h / 2, FS_M, FW_BOLD, with_alpha(rgb(0x111111), a), 600);
}

static void draw_main(void)
{
    bool overlay = A.np_open || A.sheet.open;
    ui_set_enabled(!overlay);

    rectf content = rect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H);
    gfx_push_clip(content);
    ui_push_clip(content);
    view *v = app_view();
    switch (v->type) {
    case VIEW_HOME: screen_home(v); break;
    case VIEW_SEARCH: screen_search(v); break;
    case VIEW_LIBRARY: screen_library(v); break;
    case VIEW_PAGE: screen_page(v); break;
    }
    ui_pop_clip();
    gfx_pop_clip();

    screen_sidebar();
    screen_player_bar();

    float k = A.dt * 9 > 1 ? 1 : A.dt * 9;
    A.np_anim += ((A.np_open ? 1.0f : 0.0f) - A.np_anim) * k;
    if (A.np_open && A.np_anim > 0.995f) A.np_anim = 1;
    if (!A.np_open && A.np_anim < 0.005f) A.np_anim = 0;
    A.sheet.anim += ((A.sheet.open ? 1.0f : 0.0f) - A.sheet.anim) * (A.dt * 14 > 1 ? 1 : A.dt * 14);
    if (!A.sheet.open && A.sheet.anim < 0.01f) A.sheet.anim = 0;

    if (A.np_anim > 0) {
        ui_set_enabled(A.np_open && !A.sheet.open);
        screen_now_playing();
    }
    if (A.sheet.anim > 0) {
        ui_set_enabled(A.sheet.open);
        screen_sheet();
    }
    ui_set_enabled(true);
}

void app_frame(float dt)
{
    A.dt = dt;
    A.time += dt;

    vam_event ev;
    for (int i = 0; i < 64 && vam_poll_event(&ev); i++) {
        handle_vam_event(&ev);
        vam_event_release(&ev);
    }
    /* Snapshot every frame: cheap, and keeps the progress bar honest. */
    vam_get_player_state(&A.player);
    if (!A.player.has_track && A.np_open)
        app_set_now_playing(false);

    if (A.typing && !platform_is_vita() && g_edit_changed_at >= 0 && A.time - g_edit_changed_at > 0.45f) {
        g_edit_changed_at = -1;
        run_search(A.edit, false);
    }

    images_update();
    text_begin_frame();
    gfx_clear(rgb(C_BG));

    if (A.auth.status != VAM_AUTH_LOGGED_IN) {
        screen_login();
    } else if (!A.auth.is_premium) {
        screen_premium();
    } else {
        global_buttons();
        draw_main();
    }
    draw_toast();
    apply_nav();
    platform_frame(app_is_playing());
}
