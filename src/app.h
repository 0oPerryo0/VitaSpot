#ifndef VAM_APP_H
#define VAM_APP_H

#include <SDL.h>
#include <stdbool.h>

#include "gfx/gfx.h"
#include "model.h"
#include "ui/ui.h"
#include "vam_backend.h"

#define TAB_HOME 0
#define TAB_SEARCH 1
#define TAB_LIBRARY 2
#define TAB_COUNT 3
#define MAX_DEPTH 12
#define MAX_SHELVES 16
#define RECENT_SEARCHES 8

typedef enum view_type { VIEW_HOME, VIEW_SEARCH, VIEW_LIBRARY, VIEW_PAGE } view_type;

typedef enum lib_filter { LIB_ALL, LIB_PLAYLISTS, LIB_ALBUMS, LIB_ARTISTS, LIB_FILTER_COUNT } lib_filter;

typedef struct view {
    view_type type;
    char uri[VAM_URI_MAX];
    model_page *page;
    uint32_t req;          /* pending first-page request          */
    uint32_t more_req;     /* pending pagination request          */
    bool failed;
    char error[VAM_MSG_MAX];
    scroller scroll;
    scroller shelves[MAX_SHELVES];
    uint32_t saved_focus;
    int filter;            /* VIEW_LIBRARY: lib_filter            */
    char query[256];       /* VIEW_SEARCH: committed query        */
    float content_h;       /* measured last frame, for scrolling  */
    uint32_t player_seq;   /* queue page: refresh on player change */
} view;

typedef enum nav_kind { NAV_NONE, NAV_PUSH, NAV_POP, NAV_TAB } nav_kind;

typedef struct sheet {
    bool open;
    float anim;
    vam_item item;
    char context_uri[VAM_URI_MAX];
    uint32_t index;
    bool from_page_header;
} sheet;

typedef struct app {
    SDL_Window *window;
    SDL_Renderer *renderer;
    bool quit;
    float time;      /* seconds since start, for animation */
    float dt;

    int tab;
    view stacks[TAB_COUNT][MAX_DEPTH];
    int depth[TAB_COUNT];

    /* Deferred navigation, applied after the frame is drawn. */
    nav_kind nav;
    char nav_uri[VAM_URI_MAX];
    int nav_tab;

    bool np_open;     /* Now Playing overlay */
    float np_anim;
    sheet sheet;

    /* Search text entry */
    bool typing;
    char edit[256];
    char recent[RECENT_SEARCHES][256];

    /* Toast */
    char toast[VAM_MSG_MAX];
    float toast_until;

    /* Backend state */
    vam_auth_state auth;
    vam_player_state player;
    bool have_player;

    /* Seek / volume scrubbing (owner = slider id being dragged) */
    uint32_t slider_owner;
    uint64_t slider_release_ms;  /* when the owner's pointer lifted; 0 = held */
    float scrub_frac;
    float vol_frac;

    /* Dominant colour of the current track art, eased between tracks. */
    color np_color;
} app;

extern app A;

/* app.c */
bool app_init(SDL_Window *w, SDL_Renderer *r);
void app_shutdown(void);
void app_handle_sdl_event(const SDL_Event *e);
void app_frame(float dt);

view *app_view(void);
int app_depth(void);
void app_open_uri(const char *uri);
void app_go_back(void);
void app_switch_tab(int tab);
void app_toast(const char *msg);
void app_open_sheet(const vam_item *item, const char *context_uri, uint32_t index);
void app_close_sheet(void);
void app_set_now_playing(bool open);
void app_set_search(const char *query);
void app_request_view(view *v);
void app_request_more(view *v);
void app_begin_typing(void);
uint32_t app_position_ms(void);
bool app_is_playing(void);
bool app_is_current_track(const char *uri);
bool app_is_current_context(const char *uri);
void app_toggle_saved(const char *uri, bool currently_saved);
void app_play_item(const vam_item *it, const char *context_uri, uint32_t index);

/* Shared widgets: widgets.c */
void fmt_duration(uint32_t ms, char *buf, size_t n);
void draw_cover(const char *url, rectf r, float radius, int kind, const char *title);
bool icon_button(uint32_t id, icon_id icon, float cx, float cy, float size, float hit, color c);
bool round_button(uint32_t id, icon_id icon, float cx, float cy, float radius, color bg, color fg);
bool pill_button(uint32_t id, const char *label, float x, float y, bool active, float *out_w);
void draw_focus_ring(rectf r, float radius);
void draw_back_button(view *v, float x, float y);
void draw_loading(rectf area);
bool draw_error(rectf area, const char *msg);
const char *kind_label(int kind);

/* Sections renderer: sections.c */
typedef struct sections_ctx {
    view *v;
    const char *context_uri;   /* play context for LIST rows, or NULL   */
    bool numbered;             /* show 1,2,3 instead of art             */
    bool show_art;
    rectf viewport;            /* visible content area                  */
    float x, w;                /* horizontal extent                     */
} sections_ctx;

float draw_sections(sections_ctx *c, model_page *p, uint32_t first, float y);
float draw_track_row(sections_ctx *c, const vam_item *it, uint32_t index, float y, uint32_t id_base);
float draw_entity_row(sections_ctx *c, const vam_item *it, uint32_t index, float y, uint32_t id_base);
/* Horizontal slider. Returns true when the value is committed. */
bool draw_slider(uint32_t id, rectf track, float *frac, float display_frac, float step);

/* Screens */
void screen_home(view *v);
void screen_search(view *v);
void screen_library(view *v);
void screen_page(view *v);
void screen_sidebar(void);
void screen_player_bar(void);
void screen_now_playing(void);
void screen_sheet(void);
void screen_login(void);
void screen_premium(void);

#endif
