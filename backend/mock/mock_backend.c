/*
 * Mock implementation of vam_backend.h for UI development on desktop.
 * Fake catalogue, simulated latency, generated cover images, and a player
 * that advances in real time. No network, no audio.
 *
 * Env vars:
 *   VAM_MOCK_INSTANT=1    skip the login sequence
 *   VAM_MOCK_FREE=1       log in as a non-Premium account
 *   VAM_MOCK_STAY_OUT=1   never log in (login screen work)
 */

#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vam_backend.h"

#define N_ARTISTS 8
#define N_ALBUMS 14
#define N_PLAYLISTS 9
#define MAX_TRACKS 256
#define MAX_PL_TRACKS 320
#define MAX_PENDING 256
#define MAX_QUEUE 64
#define MAX_CTX 512

typedef struct mtrack {
    char uri[VAM_URI_MAX], title[VAM_TEXT_MAX];
    int artist, album;
    uint32_t dur;
    bool expl, saved, playable;
} mtrack;

typedef struct martist { char uri[VAM_URI_MAX], name[VAM_TEXT_MAX]; bool saved; } martist;

typedef struct malbum {
    char uri[VAM_URI_MAX], title[VAM_TEXT_MAX];
    int artist, year, first, n;
    bool saved;
} malbum;

typedef struct mplaylist {
    char uri[VAM_URI_MAX], title[VAM_TEXT_MAX], owner[64], desc[VAM_DESC_MAX];
    int tracks[MAX_PL_TRACKS];
    int n;
    bool saved;
} mplaylist;

typedef struct pending { vam_event ev; uint64_t due; } pending;

static martist g_artists[N_ARTISTS];
static malbum g_albums[N_ALBUMS];
static mtrack g_tracks[MAX_TRACKS];
static int g_ntracks;
static mplaylist g_pls[N_PLAYLISTS];
static int g_liked[MAX_TRACKS];

static pending g_pending[MAX_PENDING];
static int g_npending;
static uint32_t g_next_req = 1;

static vam_auth_state g_auth;
static vam_player_state g_player;
static uint64_t g_start;
static int g_auth_stage;
static bool g_free, g_stay_out;

/* Playback context */
static int g_ctx[MAX_CTX];
static int g_nctx, g_cur;
static int g_queue[MAX_QUEUE];
static int g_nqueue;
static int g_cur_track = -1;
static uint64_t g_buffer_until;

static const char *ARTISTS[N_ARTISTS] = {
    "Northbound Atlas", "Midnight Orchard", "Pixel Lanterns", "Saltwater Choir",
    "Kiro Amano", "\xE9\x99\xB3\xE6\x99\x93\xE9\x9B\xA8", "Harbor Lights Ensemble", "Velvet Static",
};
static const char *ADJ[] = { "Glass", "Paper", "Silver", "Quiet", "Neon", "Golden", "Hollow", "Electric",
                             "Velvet", "Distant", "Broken", "Midnight", "Paper", "Wild", "Slow", "Crimson" };
static const char *NOUN[] = { "Horizon", "Satellites", "Rivers", "Summer", "Signals", "Lanterns", "Gardens",
                              "Echoes", "Highway", "Tides", "Static", "Postcards", "Comets", "Harbor",
                              "Weather", "Kingdoms" };
static const char *CJK_TITLES[] = {
    "\xE5\xA4\x9C\xE7\xA9\xBA\xE3\x81\xAE\xE3\x83\xA1\xE3\x83\xAD\xE3\x83\x87\xE3\x82\xA3", /* 夜空のメロディ */
    "\xE5\x9F\x8E\xE5\xB8\x82\xE4\xB9\x8B\xE5\x85\x89",                                     /* 城市之光 */
    "\xEB\xB0\x94\xEB\x8B\xA4\xEC\x9D\x98 \xEB\x85\xB8\xEB\x9E\x98",                         /* 바다의 노래 */
};

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void cpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s); }

static void build_catalogue(void)
{
    for (int a = 0; a < N_ARTISTS; a++) {
        snprintf(g_artists[a].uri, VAM_URI_MAX, "spotify:artist:mock%02d", a);
        cpy(g_artists[a].name, VAM_TEXT_MAX, ARTISTS[a]);
        g_artists[a].saved = a % 3 == 0;
    }
    g_ntracks = 0;
    for (int al = 0; al < N_ALBUMS; al++) {
        malbum *m = &g_albums[al];
        snprintf(m->uri, VAM_URI_MAX, "spotify:album:mock%02d", al);
        snprintf(m->title, VAM_TEXT_MAX, "%s %s", ADJ[(al * 7 + 3) % 16], NOUN[(al * 5 + 1) % 16]);
        if (al == 5) cpy(m->title, VAM_TEXT_MAX, CJK_TITLES[1]);
        m->artist = al % N_ARTISTS;
        m->year = 2008 + (al * 3) % 17;
        m->first = g_ntracks;
        m->n = 7 + (int)(rnd() % 8);
        m->saved = al % 4 == 1;
        for (int t = 0; t < m->n && g_ntracks < MAX_TRACKS; t++) {
            mtrack *tr = &g_tracks[g_ntracks];
            snprintf(tr->uri, VAM_URI_MAX, "spotify:track:mock%03d", g_ntracks);
            if (g_ntracks % 37 == 5)
                cpy(tr->title, VAM_TEXT_MAX, CJK_TITLES[g_ntracks % 3]);
            else if (g_ntracks % 11 == 0)
                snprintf(tr->title, VAM_TEXT_MAX, "%s %s (Extended Version with a Very Long Title)",
                         ADJ[rnd() % 16], NOUN[rnd() % 16]);
            else
                snprintf(tr->title, VAM_TEXT_MAX, "%s %s", ADJ[rnd() % 16], NOUN[rnd() % 16]);
            tr->artist = m->artist;
            tr->album = al;
            tr->dur = 140000 + rnd() % 150000;
            tr->expl = rnd() % 6 == 0;
            tr->playable = g_ntracks % 29 != 7;
            tr->saved = rnd() % 4 == 0;
            g_ntracks++;
        }
        m->n = g_ntracks - m->first;
    }
    static const char *pl_titles[N_PLAYLISTS] = {
        "Morning Focus", "Late Night Drive", "Rainy Day Mix", "Daily Mix 1", "Daily Mix 2",
        "Chill Vibes", "Road Trip", "Deep Work", "Everything (big playlist)",
    };
    for (int p = 0; p < N_PLAYLISTS; p++) {
        mplaylist *pl = &g_pls[p];
        snprintf(pl->uri, VAM_URI_MAX, "spotify:playlist:mock%02d", p);
        cpy(pl->title, VAM_TEXT_MAX, pl_titles[p]);
        cpy(pl->owner, sizeof(pl->owner), p < 3 ? "You" : "VitaSpot Mock");
        if (p == 1)
            cpy(pl->desc, VAM_DESC_MAX, "Long roads, empty streets and synths. A mock playlist with a description "
                                        "long enough to wrap onto a second line in the header.");
        pl->n = p == N_PLAYLISTS - 1 ? MAX_PL_TRACKS : 12 + (int)(rnd() % 40);
        for (int i = 0; i < pl->n; i++)
            pl->tracks[i] = p == N_PLAYLISTS - 1 ? i % g_ntracks : (int)(rnd() % g_ntracks);
        pl->saved = true;
    }
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

static uint64_t now_ms(void) { return SDL_GetTicks64(); }

static void push(const vam_event *ev, uint32_t delay_ms)
{
    if (g_npending >= MAX_PENDING)
        return;
    g_pending[g_npending].ev = *ev;
    g_pending[g_npending].due = now_ms() + delay_ms;
    g_npending++;
}

static void push_simple(int type, uint32_t delay)
{
    vam_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    push(&ev, delay);
}

static void player_changed(void)
{
    g_player.seq++;
    push_simple(VAM_EV_PLAYER_CHANGED, 0);
}

/* ------------------------------------------------------------------------ */
/* Items                                                                     */
/* ------------------------------------------------------------------------ */

static void img_url(char *out, char kind, int i) { snprintf(out, VAM_URL_MAX, "mock://img/%c%d", kind, i); }

static vam_item track_item(int t)
{
    vam_item it;
    memset(&it, 0, sizeof(it));
    mtrack *tr = &g_tracks[t];
    it.kind = VAM_KIND_TRACK;
    cpy(it.uri, sizeof(it.uri), tr->uri);
    cpy(it.title, sizeof(it.title), tr->title);
    cpy(it.subtitle, sizeof(it.subtitle), g_artists[tr->artist].name);
    img_url(it.image_url, 'b', tr->album);
    cpy(it.album_uri, sizeof(it.album_uri), g_albums[tr->album].uri);
    cpy(it.artist_uri, sizeof(it.artist_uri), g_artists[tr->artist].uri);
    it.duration_ms = tr->dur;
    it.is_explicit = tr->expl;
    it.is_playable = tr->playable;
    it.is_saved = tr->saved;
    return it;
}

static vam_item album_item(int a)
{
    vam_item it;
    memset(&it, 0, sizeof(it));
    it.kind = VAM_KIND_ALBUM;
    cpy(it.uri, sizeof(it.uri), g_albums[a].uri);
    cpy(it.title, sizeof(it.title), g_albums[a].title);
    snprintf(it.subtitle, sizeof(it.subtitle), "Album \xE2\x80\xA2 %s", g_artists[g_albums[a].artist].name);
    img_url(it.image_url, 'b', a);
    cpy(it.artist_uri, sizeof(it.artist_uri), g_artists[g_albums[a].artist].uri);
    it.is_playable = 1;
    it.is_saved = g_albums[a].saved;
    return it;
}

static vam_item artist_item(int a)
{
    vam_item it;
    memset(&it, 0, sizeof(it));
    it.kind = VAM_KIND_ARTIST;
    cpy(it.uri, sizeof(it.uri), g_artists[a].uri);
    cpy(it.title, sizeof(it.title), g_artists[a].name);
    cpy(it.subtitle, sizeof(it.subtitle), "Artist");
    img_url(it.image_url, 'a', a);
    it.is_playable = 1;
    it.is_saved = g_artists[a].saved;
    return it;
}

static vam_item playlist_item(int p)
{
    vam_item it;
    memset(&it, 0, sizeof(it));
    it.kind = VAM_KIND_PLAYLIST;
    cpy(it.uri, sizeof(it.uri), g_pls[p].uri);
    cpy(it.title, sizeof(it.title), g_pls[p].title);
    snprintf(it.subtitle, sizeof(it.subtitle), "Playlist \xE2\x80\xA2 %s", g_pls[p].owner);
    if (p != 2) /* one playlist without art to show the placeholder */
        img_url(it.image_url, 'p', p);
    it.is_playable = 1;
    it.is_saved = g_pls[p].saved;
    return it;
}

static int liked_count(void)
{
    int n = 0;
    for (int t = 0; t < g_ntracks; t++)
        if (g_tracks[t].saved)
            g_liked[n++] = t;
    return n;
}

static vam_item collection_item(void)
{
    vam_item it;
    memset(&it, 0, sizeof(it));
    it.kind = VAM_KIND_COLLECTION;
    cpy(it.uri, sizeof(it.uri), "spotify:user:mock:collection");
    cpy(it.title, sizeof(it.title), "Liked Songs");
    snprintf(it.subtitle, sizeof(it.subtitle), "Playlist \xE2\x80\xA2 %d songs", liked_count());
    it.is_playable = 1;
    return it;
}

/* ------------------------------------------------------------------------ */
/* Page building                                                             */
/* ------------------------------------------------------------------------ */

typedef struct builder { vam_page *p; } builder;

static vam_page *page_new(int kind, const char *uri)
{
    vam_page *p = calloc(1, sizeof(*p));
    p->kind = kind;
    cpy(p->uri, sizeof(p->uri), uri);
    p->sections = calloc(8, sizeof(vam_section));
    return p;
}

static vam_section *section(vam_page *p, const char *title, int layout, int cap)
{
    vam_section *s = &p->sections[p->section_count++];
    cpy(s->title, sizeof(s->title), title);
    s->layout = layout;
    s->items = calloc(cap > 0 ? cap : 1, sizeof(vam_item));
    return s;
}

static void add(vam_section *s, vam_item it)
{
    s->items[s->count++] = it;
    s->total = s->offset + s->count;
}

/* Add tracks [offset, offset+limit) of a list as the paged first section. */
static void paged_tracks(vam_page *p, const int *list, int n, uint32_t offset, uint32_t limit)
{
    int cap = (int)limit;
    vam_section *s = section(p, "", VAM_LAYOUT_LIST, cap);
    s->offset = offset;
    for (int i = (int)offset; i < n && (uint32_t)(i - offset) < limit; i++)
        s->items[s->count++] = track_item(list[i]);
    s->total = (uint32_t)n;
}

static int find_prefix(const char *uri, const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(uri, prefix, n) != 0)
        return -1;
    return atoi(uri + n);
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (SDL_strncasecmp(hay, needle, n) == 0)
            return true;
    return false;
}

static vam_page *build_page(const char *uri, uint32_t offset, uint32_t limit, int *err)
{
    int idx;
    vam_page *p;
    if (limit == 0 || limit > 200)
        limit = 50;
    *err = VAM_OK;

    if (strcmp(uri, "vam:home") == 0) {
        p = page_new(VAM_KIND_HOME, uri);
        vam_section *g = section(p, "", VAM_LAYOUT_GRID, 6);
        add(g, collection_item());
        add(g, playlist_item(1));
        add(g, album_item(3));
        add(g, playlist_item(3));
        add(g, album_item(5));
        add(g, playlist_item(6));
        vam_section *s = section(p, "Recently played", VAM_LAYOUT_SHELF, N_ALBUMS);
        for (int a = 0; a < N_ALBUMS; a++)
            add(s, album_item((a * 5) % N_ALBUMS));
        s = section(p, "Made for you", VAM_LAYOUT_SHELF, N_PLAYLISTS);
        for (int i = 3; i < N_PLAYLISTS; i++)
            add(s, playlist_item(i));
        s = section(p, "Your top artists", VAM_LAYOUT_SHELF, N_ARTISTS);
        for (int a = 0; a < N_ARTISTS; a++)
            add(s, artist_item(a));
        s = section(p, "Your playlists", VAM_LAYOUT_SHELF, N_PLAYLISTS);
        for (int i = 0; i < 3; i++)
            add(s, playlist_item(i));
        return p;
    }
    if (strcmp(uri, "vam:library") == 0) {
        p = page_new(VAM_KIND_LIBRARY, uri);
        vam_section *s = section(p, "", VAM_LAYOUT_LIST, 1 + N_PLAYLISTS + N_ALBUMS + N_ARTISTS);
        add(s, collection_item());
        for (int i = 0; i < N_PLAYLISTS; i++)
            if (g_pls[i].saved) add(s, playlist_item(i));
        for (int a = 0; a < N_ALBUMS; a++)
            if (g_albums[a].saved) add(s, album_item(a));
        for (int a = 0; a < N_ARTISTS; a++)
            if (g_artists[a].saved) add(s, artist_item(a));
        return p;
    }
    if (strncmp(uri, "vam:search:", 11) == 0) {
        const char *q = uri + 11;
        p = page_new(VAM_KIND_SEARCH, uri);
        vam_section *s = section(p, "Songs", VAM_LAYOUT_LIST, 10);
        for (int t = 0; t < g_ntracks && s->count < 10; t++)
            if (contains_ci(g_tracks[t].title, q) || contains_ci(g_artists[g_tracks[t].artist].name, q))
                add(s, track_item(t));
        s = section(p, "Artists", VAM_LAYOUT_SHELF, N_ARTISTS);
        for (int a = 0; a < N_ARTISTS; a++)
            if (contains_ci(g_artists[a].name, q)) add(s, artist_item(a));
        s = section(p, "Albums", VAM_LAYOUT_SHELF, N_ALBUMS);
        for (int a = 0; a < N_ALBUMS; a++)
            if (contains_ci(g_albums[a].title, q) || contains_ci(g_artists[g_albums[a].artist].name, q))
                add(s, album_item(a));
        s = section(p, "Playlists", VAM_LAYOUT_SHELF, N_PLAYLISTS);
        for (int i = 0; i < N_PLAYLISTS; i++)
            if (contains_ci(g_pls[i].title, q)) add(s, playlist_item(i));
        return p;
    }
    if (strcmp(uri, "vam:queue") == 0) {
        p = page_new(VAM_KIND_QUEUE, uri);
        vam_section *s = section(p, "Now playing", VAM_LAYOUT_LIST, 1);
        if (g_cur_track >= 0)
            add(s, track_item(g_cur_track));
        s = section(p, "Next up", VAM_LAYOUT_LIST, MAX_QUEUE + 30);
        for (int i = 0; i < g_nqueue; i++)
            add(s, track_item(g_queue[i]));
        for (int i = g_cur + 1; i < g_nctx && s->count < (uint32_t)(MAX_QUEUE + 30); i++)
            add(s, track_item(g_ctx[i]));
        return p;
    }
    if (strcmp(uri, "spotify:user:mock:collection") == 0) {
        p = page_new(VAM_KIND_COLLECTION, uri);
        int n = liked_count();
        cpy(p->title, sizeof(p->title), "Liked Songs");
        snprintf(p->subtitle, sizeof(p->subtitle), "You \xE2\x80\xA2 %d songs", n);
        paged_tracks(p, g_liked, n, offset, limit);
        return p;
    }
    if ((idx = find_prefix(uri, "spotify:album:mock")) >= 0 && idx < N_ALBUMS) {
        malbum *m = &g_albums[idx];
        p = page_new(VAM_KIND_ALBUM, uri);
        cpy(p->title, sizeof(p->title), m->title);
        snprintf(p->subtitle, sizeof(p->subtitle), "%s \xE2\x80\xA2 %d \xE2\x80\xA2 %d songs",
                 g_artists[m->artist].name, m->year, m->n);
        img_url(p->image_url, 'b', idx);
        p->is_saved = m->saved;
        int list[64];
        for (int i = 0; i < m->n; i++)
            list[i] = m->first + i;
        paged_tracks(p, list, m->n, offset, limit);
        return p;
    }
    if ((idx = find_prefix(uri, "spotify:playlist:mock")) >= 0 && idx < N_PLAYLISTS) {
        mplaylist *pl = &g_pls[idx];
        p = page_new(VAM_KIND_PLAYLIST, uri);
        cpy(p->title, sizeof(p->title), pl->title);
        cpy(p->description, sizeof(p->description), pl->desc);
        snprintf(p->subtitle, sizeof(p->subtitle), "%s \xE2\x80\xA2 %d songs", pl->owner, pl->n);
        if (idx != 2)
            img_url(p->image_url, 'p', idx);
        p->is_saved = pl->saved;
        paged_tracks(p, pl->tracks, pl->n, offset, limit);
        return p;
    }
    if ((idx = find_prefix(uri, "spotify:artist:mock")) >= 0 && idx < N_ARTISTS) {
        p = page_new(VAM_KIND_ARTIST, uri);
        cpy(p->title, sizeof(p->title), g_artists[idx].name);
        snprintf(p->subtitle, sizeof(p->subtitle), "%u monthly listeners", 10000 + idx * 73411);
        img_url(p->image_url, 'a', idx);
        p->is_saved = g_artists[idx].saved;
        int list[MAX_TRACKS], n = 0;
        for (int t = 0; t < g_ntracks && n < 10; t++)
            if (g_tracks[t].artist == idx) list[n++] = t;
        vam_section *s = section(p, "Popular", VAM_LAYOUT_LIST, n);
        for (int i = 0; i < n; i++) add(s, track_item(list[i]));
        s = section(p, "Albums", VAM_LAYOUT_SHELF, N_ALBUMS);
        for (int a = 0; a < N_ALBUMS; a++)
            if (g_albums[a].artist == idx) add(s, album_item(a));
        s = section(p, "Fans also like", VAM_LAYOUT_SHELF, N_ARTISTS);
        for (int a = 0; a < N_ARTISTS; a++)
            if (a != idx) add(s, artist_item(a));
        return p;
    }
    *err = VAM_ERR_NOT_FOUND;
    return NULL;
}

/* Fills ctx with the track list of a context URI. */
static int context_tracks(const char *uri, int *out)
{
    int idx, n = 0;
    if (strcmp(uri, "spotify:user:mock:collection") == 0) {
        n = liked_count();
        memcpy(out, g_liked, sizeof(int) * n);
    } else if ((idx = find_prefix(uri, "spotify:album:mock")) >= 0 && idx < N_ALBUMS) {
        for (int i = 0; i < g_albums[idx].n; i++) out[n++] = g_albums[idx].first + i;
    } else if ((idx = find_prefix(uri, "spotify:playlist:mock")) >= 0 && idx < N_PLAYLISTS) {
        n = g_pls[idx].n;
        memcpy(out, g_pls[idx].tracks, sizeof(int) * n);
    } else if ((idx = find_prefix(uri, "spotify:artist:mock")) >= 0 && idx < N_ARTISTS) {
        for (int t = 0; t < g_ntracks && n < MAX_CTX; t++)
            if (g_tracks[t].artist == idx) out[n++] = t;
    }
    return n;
}

static const char *context_title(const char *uri)
{
    int idx;
    if (strstr(uri, "collection")) return "Liked Songs";
    if ((idx = find_prefix(uri, "spotify:album:mock")) >= 0 && idx < N_ALBUMS) return g_albums[idx].title;
    if ((idx = find_prefix(uri, "spotify:playlist:mock")) >= 0 && idx < N_PLAYLISTS) return g_pls[idx].title;
    if ((idx = find_prefix(uri, "spotify:artist:mock")) >= 0 && idx < N_ARTISTS) return g_artists[idx].name;
    return "";
}

static int track_index(const char *uri)
{
    int idx = find_prefix(uri, "spotify:track:mock");
    return idx >= 0 && idx < g_ntracks ? idx : -1;
}

/* ------------------------------------------------------------------------ */
/* Images: tiny generated BMPs                                               */
/* ------------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static uint8_t *make_image(const char *url, uint32_t *size)
{
    /* Profile pictures ("u") are wide with a round dot in the middle, so a
       stretched draw shows as an oval. */
    bool profile = strncmp(url, "mock://img/u", 12) == 0;
    const int W = profile ? 240 : 160, H = 160;
    uint32_t seed = 2166136261u;
    for (const char *c = url; *c; c++) seed = (seed ^ (uint8_t)*c) * 16777619u;
    uint8_t c1[3] = { (uint8_t)(seed), (uint8_t)(seed >> 8), (uint8_t)(seed >> 16) };
    uint8_t c2[3] = { (uint8_t)(255 - c1[1]), (uint8_t)(c1[2] / 2 + 60), (uint8_t)(c1[0] / 3 + 40) };
    int row = W * 3;
    uint32_t total = 54 + row * H;
    uint8_t *b = calloc(1, total);
    b[0] = 'B'; b[1] = 'M';
    put32(b + 2, total);
    put32(b + 10, 54);
    put32(b + 14, 40);
    put32(b + 18, W);
    put32(b + 22, H);
    put16(b + 26, 1);
    put16(b + 28, 24);
    put32(b + 34, row * H);
    int cx = (int)(seed >> 24) % W, cy = (int)(seed >> 20) % H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float t = (x + (H - 1 - y)) / (float)(W + H);
            int dx = x - cx, dy = (H - 1 - y) - cy;
            bool ring = (dx * dx + dy * dy) / 220 % 3 == 0;
            uint8_t *px = b + 54 + y * row + x * 3;
            for (int k = 0; k < 3; k++) {
                float v = c1[k] * (1 - t) + c2[k] * t;
                if (ring) v = v * 0.8f + 40;
                if (profile && (x - W / 2) * (x - W / 2) + (y - H / 2) * (y - H / 2) < 40 * 40) v = 245;
                px[2 - k] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
    *size = total;
    return b;
}

/* ------------------------------------------------------------------------ */
/* Player                                                                    */
/* ------------------------------------------------------------------------ */

static void load_track(int t, bool play)
{
    g_cur_track = t;
    g_player.has_track = 1;
    g_player.track = track_item(t);
    g_player.position_ms = 0;
    g_player.position_timestamp_ms = now_ms();
    g_player.playback = play ? VAM_PLAYBACK_BUFFERING : VAM_PLAYBACK_PAUSED;
    g_buffer_until = play ? now_ms() + 350 : 0;
    player_changed();
}

static uint32_t position_now(void)
{
    uint64_t p = g_player.position_ms;
    if (g_player.playback == VAM_PLAYBACK_PLAYING)
        p += now_ms() - g_player.position_timestamp_ms;
    return (uint32_t)p;
}

static void freeze_position(void)
{
    g_player.position_ms = position_now();
    g_player.position_timestamp_ms = now_ms();
}

static bool advance(bool user)
{
    if (g_nqueue > 0) {
        int t = g_queue[0];
        memmove(g_queue, g_queue + 1, sizeof(int) * --g_nqueue);
        load_track(t, true);
        return true;
    }
    if (!user && g_player.repeat == VAM_REPEAT_TRACK) {
        load_track(g_cur_track, true);
        return true;
    }
    if (g_nctx == 0)
        return false;
    if (g_player.shuffle && g_nctx > 1) {
        int n;
        do n = (int)(rnd() % g_nctx); while (n == g_cur);
        g_cur = n;
    } else if (g_cur + 1 < g_nctx) {
        g_cur++;
    } else if (g_player.repeat != VAM_REPEAT_OFF) {
        g_cur = 0;
    } else {
        return false;
    }
    load_track(g_ctx[g_cur], true);
    return true;
}

static void tick_player(void)
{
    if (g_player.playback == VAM_PLAYBACK_BUFFERING && now_ms() >= g_buffer_until) {
        g_player.playback = VAM_PLAYBACK_PLAYING;
        g_player.position_timestamp_ms = now_ms();
        player_changed();
    }
    if (g_player.playback == VAM_PLAYBACK_PLAYING && position_now() >= g_player.track.duration_ms) {
        if (!advance(false)) {
            g_player.playback = VAM_PLAYBACK_PAUSED;
            g_player.position_ms = 0;
            g_player.position_timestamp_ms = now_ms();
            player_changed();
        }
    }
}

static void tick_auth(void)
{
    uint64_t t = now_ms() - g_start;
    int stage = g_auth_stage;
    if (stage == 0 && t > 700) stage = 1;
    if (stage == 1 && t > 2600 && !g_stay_out) stage = 2;
    if (stage == 2 && t > 3600) stage = 3;
    if (stage == g_auth_stage)
        return;
    g_auth_stage = stage;
    switch (stage) {
    case 1:
        g_auth.status = VAM_AUTH_LOGGED_OUT;
        g_auth.zeroconf_active = 1;
        cpy(g_auth.message, sizeof(g_auth.message), "Mock backend: waiting for a Spotify app on your network.");
        break;
    case 2:
        g_auth.status = VAM_AUTH_CONNECTING;
        cpy(g_auth.message, sizeof(g_auth.message), "Received credentials from \"Mock Phone\".");
        break;
    case 3:
        g_auth.status = VAM_AUTH_LOGGED_IN;
        g_auth.is_premium = !g_free;
        cpy(g_auth.username, sizeof(g_auth.username), "mockuser");
        cpy(g_auth.display_name, sizeof(g_auth.display_name), "Mock User");
        /* Wide on purpose: profile photos needn't be square. */
        img_url(g_auth.image_url, 'u', 0);
        g_auth.message[0] = 0;
        break;
    }
    push_simple(VAM_EV_AUTH_CHANGED, 0);
}

/* ------------------------------------------------------------------------ */
/* API                                                                       */
/* ------------------------------------------------------------------------ */

vam_result vam_init(const vam_config *config)
{
    if (!config || config->api_version != VAM_API_VERSION)
        return VAM_ERR_INVALID_ARG;
    build_catalogue();
    memset(&g_auth, 0, sizeof(g_auth));
    memset(&g_player, 0, sizeof(g_player));
    cpy(g_auth.device_name, sizeof(g_auth.device_name), config->device_name ? config->device_name : "VitaSpot");
    g_auth.status = VAM_AUTH_STARTING;
    g_player.volume = config->initial_volume;
    g_player.is_active_device = 1;
    g_start = now_ms();
    g_free = SDL_getenv("VAM_MOCK_FREE") != NULL;
    g_stay_out = SDL_getenv("VAM_MOCK_STAY_OUT") != NULL;
    if (SDL_getenv("VAM_MOCK_INSTANT")) {
        g_start -= 10000;
        g_auth_stage = 2;
    }
    return VAM_OK;
}

void vam_shutdown(void)
{
    for (int i = 0; i < g_npending; i++)
        vam_event_release(&g_pending[i].ev);
    g_npending = 0;
}

uint64_t vam_now_ms(void) { return now_ms(); }

void vam_log(const char *message)
{
    if (message && *message)
        SDL_Log("ui: %s", message);
}

int vam_poll_event(vam_event *out)
{
    tick_auth();
    tick_player();
    uint64_t now = now_ms();
    for (int i = 0; i < g_npending; i++) {
        if (g_pending[i].due <= now) {
            *out = g_pending[i].ev;
            memmove(&g_pending[i], &g_pending[i + 1], sizeof(pending) * (g_npending - i - 1));
            g_npending--;
            return 1;
        }
    }
    return 0;
}

void vam_event_release(vam_event *ev)
{
    if (ev->page) {
        for (uint32_t i = 0; i < ev->page->section_count; i++)
            free(ev->page->sections[i].items);
        free(ev->page->sections);
        free(ev->page);
        ev->page = NULL;
    }
    if (ev->data) {
        free((void *)ev->data);
        ev->data = NULL;
    }
}

void vam_get_auth_state(vam_auth_state *out) { *out = g_auth; }
void vam_get_player_state(vam_player_state *out) { *out = g_player; }

vam_result vam_logout(void)
{
    g_auth.status = VAM_AUTH_LOGGED_OUT;
    g_auth.is_premium = 0;
    g_auth_stage = 1;
    g_stay_out = true;
    cpy(g_auth.message, sizeof(g_auth.message), "Logged out.");
    push_simple(VAM_EV_AUTH_CHANGED, 0);
    return VAM_OK;
}

uint32_t vam_request_page(const char *uri, uint32_t offset, uint32_t limit)
{
    vam_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = VAM_EV_PAGE;
    ev.request_id = g_next_req++;
    cpy(ev.uri, sizeof(ev.uri), uri);
    int err;
    ev.page = build_page(uri, offset, limit, &err);
    ev.result = err;
    if (!ev.page)
        cpy(ev.message, sizeof(ev.message), "Mock backend doesn't know this page.");
    push(&ev, 200 + rnd() % 400);
    return ev.request_id;
}

uint32_t vam_request_image(const char *url)
{
    vam_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = VAM_EV_IMAGE;
    ev.request_id = g_next_req++;
    if (strncmp(url, "mock://img/", 11) == 0) {
        ev.data = make_image(url, &ev.data_size);
        ev.result = VAM_OK;
    } else {
        ev.result = VAM_ERR_NOT_FOUND;
    }
    push(&ev, 80 + rnd() % 500);
    return ev.request_id;
}

void vam_cancel(uint32_t request_id)
{
    for (int i = 0; i < g_npending; i++) {
        if (g_pending[i].ev.request_id == request_id) {
            vam_event_release(&g_pending[i].ev);
            memmove(&g_pending[i], &g_pending[i + 1], sizeof(pending) * (g_npending - i - 1));
            g_npending--;
            return;
        }
    }
}

vam_result vam_play(const char *context_uri, const char *track_uri, uint32_t index)
{
    if (!g_auth.is_premium)
        return VAM_ERR_NOT_PREMIUM;
    if (context_uri && *context_uri) {
        int n = context_tracks(context_uri, g_ctx);
        if (n == 0)
            return VAM_ERR_NOT_FOUND;
        g_nctx = n;
        g_cur = (int)(index < (uint32_t)n ? index : 0);
        int t = track_uri && *track_uri ? track_index(track_uri) : -1;
        if (t >= 0) {
            for (int i = 0; i < n; i++)
                if (g_ctx[i] == t) { g_cur = i; break; }
        } else if (g_player.shuffle) {
            g_cur = (int)(rnd() % n);
        }
        cpy(g_player.context_uri, sizeof(g_player.context_uri), context_uri);
        cpy(g_player.context_title, sizeof(g_player.context_title), context_title(context_uri));
        load_track(g_ctx[g_cur], true);
        return VAM_OK;
    }
    int t = track_uri ? track_index(track_uri) : -1;
    if (t < 0)
        return VAM_ERR_NOT_FOUND;
    g_ctx[0] = t;
    g_nctx = 1;
    g_cur = 0;
    g_player.context_uri[0] = 0;
    g_player.context_title[0] = 0;
    load_track(t, true);
    return VAM_OK;
}

vam_result vam_pause(void)
{
    if (!g_player.has_track) return VAM_ERR_INVALID_ARG;
    freeze_position();
    g_player.playback = VAM_PLAYBACK_PAUSED;
    player_changed();
    return VAM_OK;
}

vam_result vam_resume(void)
{
    if (!g_player.has_track) return VAM_ERR_INVALID_ARG;
    g_player.position_timestamp_ms = now_ms();
    g_player.playback = VAM_PLAYBACK_PLAYING;
    player_changed();
    return VAM_OK;
}

vam_result vam_next(void)
{
    if (!advance(true)) {
        vam_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = VAM_EV_TOAST;
        cpy(ev.message, sizeof(ev.message), "End of the list.");
        push(&ev, 0);
    }
    return VAM_OK;
}

vam_result vam_prev(void)
{
    if (!g_player.has_track) return VAM_ERR_INVALID_ARG;
    if (position_now() > 3000 || g_cur == 0 || g_nctx == 0) {
        g_player.position_ms = 0;
        g_player.position_timestamp_ms = now_ms();
        player_changed();
        return VAM_OK;
    }
    g_cur--;
    load_track(g_ctx[g_cur], true);
    return VAM_OK;
}

vam_result vam_seek(uint32_t pos)
{
    if (!g_player.has_track) return VAM_ERR_INVALID_ARG;
    g_player.position_ms = pos < g_player.track.duration_ms ? pos : g_player.track.duration_ms;
    g_player.position_timestamp_ms = now_ms();
    player_changed();
    return VAM_OK;
}

vam_result vam_set_shuffle(int enabled)
{
    g_player.shuffle = enabled != 0;
    player_changed();
    return VAM_OK;
}

vam_result vam_set_repeat(int32_t mode)
{
    g_player.repeat = mode;
    player_changed();
    return VAM_OK;
}

vam_result vam_set_volume(uint16_t volume)
{
    if (g_player.volume == volume) return VAM_OK;
    g_player.volume = volume;
    player_changed();
    return VAM_OK;
}

vam_result vam_add_to_queue(const char *track_uri)
{
    int t = track_index(track_uri);
    if (t < 0 || g_nqueue >= MAX_QUEUE) return VAM_ERR_INVALID_ARG;
    g_queue[g_nqueue++] = t;
    player_changed();
    return VAM_OK;
}

uint32_t vam_set_saved(const char *uri, int saved)
{
    int idx;
    vam_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = VAM_EV_REQUEST_DONE;
    ev.request_id = g_next_req++;
    cpy(ev.uri, sizeof(ev.uri), uri);
    ev.result = VAM_OK;
    if ((idx = track_index(uri)) >= 0) g_tracks[idx].saved = saved;
    else if ((idx = find_prefix(uri, "spotify:album:mock")) >= 0 && idx < N_ALBUMS) g_albums[idx].saved = saved;
    else if ((idx = find_prefix(uri, "spotify:playlist:mock")) >= 0 && idx < N_PLAYLISTS) g_pls[idx].saved = saved;
    else if ((idx = find_prefix(uri, "spotify:artist:mock")) >= 0 && idx < N_ARTISTS) g_artists[idx].saved = saved;
    else ev.result = VAM_ERR_NOT_FOUND;
    push(&ev, 250);
    if (ev.result == VAM_OK) {
        if (g_cur_track >= 0 && strcmp(g_player.track.uri, uri) == 0) {
            g_player.track.is_saved = saved;
            g_player.seq++;
        }
        vam_event ch;
        memset(&ch, 0, sizeof(ch));
        ch.type = VAM_EV_SAVED_CHANGED;
        cpy(ch.uri, sizeof(ch.uri), uri);
        ch.saved = saved != 0;
        push(&ch, 260);
    }
    return ev.request_id;
}
