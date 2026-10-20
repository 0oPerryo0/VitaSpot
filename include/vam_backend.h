/*
 * vam_backend.h - contract between the VitaSpot frontend (SDL2 UI) and the
 * playback/metadata backend (librespot port, or the mock in backend/mock).
 *
 * Rules every implementation must follow:
 *
 *  1. Threading. The frontend calls every function from ONE thread (the UI
 *     thread). No function may block for more than ~1 ms; all network work
 *     happens on backend-owned threads. Results come back as events through
 *     vam_poll_event(), which the UI calls once or more per frame.
 *
 *  2. Memory. Anything the backend hands out (vam_page, image bytes) is
 *     allocated by the backend and released by the backend through
 *     vam_event_release(). The frontend copies what it needs and never calls
 *     free() on backend memory.
 *
 *  3. Strings. All strings are UTF-8, NUL-terminated, and truncated (on a
 *     code point boundary) to fit their fixed-size buffers.
 *
 *  4. URIs. The frontend treats URIs as opaque strings. It only ever passes
 *     back URIs it received from the backend, plus the virtual URIs listed
 *     under vam_request_page().
 *
 *  5. ABI. Plain C structs, no packing pragmas, enums stored as int32_t.
 *     A Rust implementation should mirror these with #[repr(C)].
 */
#ifndef VAM_BACKEND_H
#define VAM_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VAM_API_VERSION 2

#define VAM_URI_MAX  96
#define VAM_TEXT_MAX 192
#define VAM_URL_MAX  256
#define VAM_MSG_MAX  256
#define VAM_DESC_MAX 512

/* ------------------------------------------------------------------------ */
/* Enums (stored as int32_t in structs)                                      */
/* ------------------------------------------------------------------------ */

typedef enum vam_result {
    VAM_OK = 0,
    VAM_ERR_UNKNOWN = 1,
    VAM_ERR_NETWORK = 2,      /* no Wi-Fi, DNS, TLS or socket failure      */
    VAM_ERR_AUTH = 3,         /* not logged in / credentials rejected      */
    VAM_ERR_NOT_PREMIUM = 4,  /* account is not Spotify Premium            */
    VAM_ERR_NOT_FOUND = 5,
    VAM_ERR_RATE_LIMITED = 6,
    VAM_ERR_UNSUPPORTED = 7,
    VAM_ERR_INVALID_ARG = 8,
    VAM_ERR_UNAVAILABLE = 9   /* track not playable in region etc.         */
} vam_result;

typedef enum vam_kind {
    VAM_KIND_UNKNOWN = 0,
    VAM_KIND_TRACK = 1,
    VAM_KIND_EPISODE = 2,
    VAM_KIND_ALBUM = 3,
    VAM_KIND_PLAYLIST = 4,
    VAM_KIND_ARTIST = 5,
    VAM_KIND_SHOW = 6,
    VAM_KIND_COLLECTION = 7,  /* the user's Liked Songs                    */
    VAM_KIND_HOME = 8,        /* page kinds for virtual URIs               */
    VAM_KIND_SEARCH = 9,
    VAM_KIND_LIBRARY = 10,
    VAM_KIND_QUEUE = 11
} vam_kind;

/* How the frontend should lay out a section. A hint, not a requirement. */
typedef enum vam_layout {
    VAM_LAYOUT_LIST = 0,   /* vertical rows (tracks, library entries)      */
    VAM_LAYOUT_SHELF = 1,  /* horizontal scrolling row of square cards     */
    VAM_LAYOUT_GRID = 2    /* compact 3-column "quick access" tiles        */
} vam_layout;

typedef enum vam_auth_status {
    VAM_AUTH_STARTING = 0,    /* backend booting, network coming up        */
    VAM_AUTH_LOGGED_OUT = 1,  /* waiting for the user (Zeroconf advertised) */
    VAM_AUTH_CONNECTING = 2,  /* credentials received, logging in          */
    VAM_AUTH_LOGGED_IN = 3,
    VAM_AUTH_ERROR = 4        /* see vam_auth_state.message                */
} vam_auth_status;

typedef enum vam_playback {
    VAM_PLAYBACK_STOPPED = 0,
    VAM_PLAYBACK_PLAYING = 1,
    VAM_PLAYBACK_PAUSED = 2,
    VAM_PLAYBACK_BUFFERING = 3
} vam_playback;

typedef enum vam_repeat {
    VAM_REPEAT_OFF = 0,
    VAM_REPEAT_CONTEXT = 1,
    VAM_REPEAT_TRACK = 2
} vam_repeat;

typedef enum vam_event_type {
    VAM_EV_NONE = 0,
    VAM_EV_AUTH_CHANGED = 1,    /* call vam_get_auth_state()               */
    VAM_EV_PLAYER_CHANGED = 2,  /* call vam_get_player_state()             */
    VAM_EV_PAGE = 3,            /* reply to vam_request_page()             */
    VAM_EV_IMAGE = 4,           /* reply to vam_request_image()            */
    VAM_EV_REQUEST_DONE = 5,    /* reply to vam_set_saved()                */
    VAM_EV_SAVED_CHANGED = 6,   /* uri's saved/followed state changed      */
    VAM_EV_ERROR = 7,           /* unsolicited error, show to the user     */
    VAM_EV_TOAST = 8,           /* informational message, show briefly     */
    VAM_EV_LYRICS = 9           /* reply to vam_request_lyrics()           */
} vam_event_type;

/* ------------------------------------------------------------------------ */
/* Data                                                                      */
/* ------------------------------------------------------------------------ */

/* One entry in any list: track, album, playlist, artist, Liked Songs... */
typedef struct vam_item {
    int32_t kind;                  /* vam_kind                              */
    char uri[VAM_URI_MAX];         /* e.g. "spotify:track:..."              */
    char title[VAM_TEXT_MAX];
    /* Second line. Tracks: "Artist A, Artist B". Albums: "Album • Artist".
     * Playlists: "Playlist • Owner". Artists: "Artist".                    */
    char subtitle[VAM_TEXT_MAX];
    /* Best image near 300x300 px (JPEG/PNG). Empty if none.                */
    char image_url[VAM_URL_MAX];
    char album_uri[VAM_URI_MAX];   /* tracks: their album; else empty       */
    char artist_uri[VAM_URI_MAX];  /* tracks/albums: first artist; else ""  */
    uint32_t duration_ms;          /* tracks/episodes; else 0               */
    uint8_t is_explicit;
    uint8_t is_playable;           /* 0 = greyed out (region lock etc.)     */
    uint8_t is_saved;              /* Liked / in library / followed         */
    uint8_t reserved;
} vam_item;

typedef struct vam_section {
    char title[VAM_TEXT_MAX];      /* "Popular", "Albums"... may be empty   */
    int32_t layout;                /* vam_layout                            */
    uint32_t offset;               /* index of items[0] in the full section */
    uint32_t total;                /* full section length (>= offset+count) */
    uint32_t count;                /* number of entries in items            */
    vam_item *items;
} vam_section;

typedef struct vam_page {
    int32_t kind;                  /* vam_kind of the page itself           */
    char uri[VAM_URI_MAX];         /* the URI that was requested            */
    char title[VAM_TEXT_MAX];
    /* Albums: "Artist • 2021 • 12 songs". Playlists: "Owner • 50 songs".   */
    char subtitle[VAM_TEXT_MAX];
    char description[VAM_DESC_MAX];/* playlist description, may be empty    */
    char image_url[VAM_URL_MAX];   /* header image, may be empty            */
    uint8_t is_saved;              /* album saved / playlist or artist followed */
    uint8_t reserved[3];
    uint32_t section_count;
    vam_section *sections;
} vam_page;

typedef struct vam_player_state {
    int32_t playback;              /* vam_playback                          */
    int32_t repeat;                /* vam_repeat                            */
    uint8_t shuffle;
    uint8_t has_track;             /* 0 = nothing loaded, `track` is empty  */
    /* 0 = another Spotify Connect device took over playback. The UI then
     * shows "Playing on another device" and still allows sending commands. */
    uint8_t is_active_device;
    uint8_t reserved;
    uint16_t volume;               /* 0..65535                              */
    uint16_t reserved2;
    /* Playback position at time `position_timestamp_ms` (vam_now_ms clock).
     * The UI extrapolates while playing, so update it on play/pause/seek/
     * track change, not every few milliseconds.                            */
    uint32_t position_ms;
    uint64_t position_timestamp_ms;
    vam_item track;                /* duration_ms must be set               */
    char context_uri[VAM_URI_MAX]; /* playlist/album/... being played       */
    char context_title[VAM_TEXT_MAX];
    uint32_t seq;                  /* incremented on every change           */
} vam_player_state;

typedef struct vam_auth_state {
    int32_t status;                /* vam_auth_status                       */
    uint8_t is_premium;            /* valid when status == LOGGED_IN        */
    uint8_t zeroconf_active;       /* device is visible in Spotify Connect  */
    uint8_t reserved[2];
    char username[VAM_TEXT_MAX];
    char display_name[VAM_TEXT_MAX];
    char device_name[VAM_TEXT_MAX];/* name shown in the phone's device list */
    char message[VAM_MSG_MAX];     /* human-readable status or error        */
    /* Optional alternative login: a URL (and short code) the user opens on
     * another device. Empty when not offered.                             */
    char login_url[VAM_URL_MAX];
    char login_code[32];
    char image_url[VAM_URL_MAX];   /* profile picture; empty if none       */
} vam_auth_state;

/* Lyrics of one track. All lines share one text buffer: line i is the
 * NUL-terminated string at text + lines[i].text_offset (may be empty for an
 * instrumental gap). Lines are in playback order. */
typedef struct vam_lyrics_line {
    uint32_t start_ms;             /* line start; 0 when not synced         */
    uint32_t text_offset;          /* byte offset into vam_lyrics.text      */
} vam_lyrics_line;

typedef struct vam_lyrics {
    uint8_t synced;                /* 1 = start_ms is valid for every line  */
    uint8_t reserved[3];
    uint32_t line_count;
    vam_lyrics_line *lines;
    uint32_t text_size;            /* bytes in text, including every NUL    */
    const char *text;
} vam_lyrics;

typedef struct vam_event {
    int32_t type;                  /* vam_event_type                        */
    uint32_t request_id;           /* 0 for unsolicited events              */
    int32_t result;                /* vam_result                            */
    char uri[VAM_URI_MAX];         /* PAGE/SAVED_CHANGED/REQUEST_DONE: subject; IMAGE: empty */
    char message[VAM_MSG_MAX];     /* ERROR/TOAST text, or failure detail   */
    uint8_t saved;                 /* SAVED_CHANGED: new state              */
    uint8_t reserved[3];
    vam_page *page;                /* PAGE on success, else NULL            */
    const uint8_t *data;           /* IMAGE on success: encoded JPEG/PNG    */
    uint32_t data_size;
    vam_lyrics *lyrics;            /* LYRICS on success, else NULL          */
} vam_event;

typedef struct vam_config {
    uint32_t api_version;          /* must be VAM_API_VERSION               */
    const char *device_name;       /* Spotify Connect name, e.g. "PS Vita"  */
    const char *data_dir;          /* writable dir with trailing slash,
                                      e.g. "ux0:data/VitaSpot/"; store cached
                                      credentials and audio cache here       */
    int32_t bitrate;               /* 96, 160 or 320 kbps                   */
    uint16_t initial_volume;       /* 0..65535                              */
    uint8_t normalize_volume;      /* loudness normalisation on/off         */
    uint8_t reserved;
} vam_config;

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

/* Start backend threads, bring up networking, restore cached credentials
 * (auto-login) or start Zeroconf discovery. Returns VAM_OK or an error;
 * on error the frontend shows `message` from vam_get_auth_state().       */
vam_result vam_init(const vam_config *config);

/* Stop playback, join threads, release everything. Called once at exit. */
void vam_shutdown(void);

/* Monotonic clock in milliseconds shared with vam_player_state. */
uint64_t vam_now_ms(void);

/* Write a line from the UI into the backend log (vam.log on the Vita). */
void vam_log(const char *message);

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

/* Non-blocking. Returns 1 and fills *out if an event was queued, else 0.
 * Each returned event must be passed to vam_event_release() exactly once. */
int vam_poll_event(vam_event *out);

/* Frees event->page / event->data / event->lyrics. Safe on any event. */
void vam_event_release(vam_event *event);

/* ------------------------------------------------------------------------ */
/* State snapshots (cheap, copy under a mutex)                               */
/* ------------------------------------------------------------------------ */

void vam_get_auth_state(vam_auth_state *out);
void vam_get_player_state(vam_player_state *out);

/* Forget stored credentials and return to VAM_AUTH_LOGGED_OUT. */
vam_result vam_logout(void);

/* ------------------------------------------------------------------------ */
/* Content requests (async). Return a non-zero request id; the reply is a   */
/* VAM_EV_PAGE / VAM_EV_IMAGE event carrying the same id. Return 0 only if  */
/* the request could not even be queued.                                     */
/* ------------------------------------------------------------------------ */

/*
 * Request a page. `offset`/`limit` page through the FIRST section only
 * (the track list of albums, playlists and Liked Songs). For offset > 0 the
 * reply needs only that section, with vam_section.offset == offset; other
 * page fields may be repeated or left empty.
 *
 * Real URIs: spotify:album:*, spotify:playlist:*, spotify:artist:*,
 *            spotify:show:*, and the Liked Songs URI the backend put in the
 *            library (e.g. "spotify:user:<name>:collection").
 *   album / playlist / collection: section 0 = tracks (LIST).
 *   artist: section 0 = "Popular" top tracks (LIST, ~10), then SHELF
 *           sections such as "Albums", "Singles and EPs", "Appears on".
 *
 * Virtual URIs:
 *   "vam:home"           Sections chosen by the backend. Suggested: a GRID of
 *                        up to 6 recent/pinned items, then SHELVES like
 *                        "Recently played", "Your playlists", "Saved albums",
 *                        "Your top artists".
 *   "vam:library"        One LIST section: Liked Songs first (kind
 *                        COLLECTION), then playlists, saved albums, followed
 *                        artists. The UI filters by item kind.
 *   "vam:search:<text>"  <text> is the raw UTF-8 query. Sections: "Songs"
 *                        (LIST), "Artists", "Albums", "Playlists" (SHELF).
 *   "vam:queue"          Section 0 "Now playing" (1 item), section 1
 *                        "Next up" (LIST).
 */
uint32_t vam_request_page(const char *uri, uint32_t offset, uint32_t limit);

/* Download an image_url. Reply data is the encoded file (JPEG/PNG). The
 * backend should keep a small disk/memory cache. */
uint32_t vam_request_image(const char *url);

/* Fetch the lyrics of a spotify:track: URI. The VAM_EV_LYRICS reply has
 * uri = track_uri and, on VAM_OK, at least one line in event->lyrics.
 * VAM_ERR_NOT_FOUND means the track has no lyrics. */
uint32_t vam_request_lyrics(const char *track_uri);

/* Best effort: drop the reply of a request the UI no longer needs. */
void vam_cancel(uint32_t request_id);

/* ------------------------------------------------------------------------ */
/* Playback commands (async, return immediately). Outcome is visible via    */
/* VAM_EV_PLAYER_CHANGED; failures via VAM_EV_ERROR.                         */
/* ------------------------------------------------------------------------ */

/*
 * Start playback.
 *   context_uri non-empty: play that album/playlist/artist/collection,
 *     starting at `track_uri` if non-empty, else at `index` (0-based),
 *     honouring the current shuffle setting.
 *   context_uri NULL or empty: play the single `track_uri`.
 */
vam_result vam_play(const char *context_uri, const char *track_uri, uint32_t index);
vam_result vam_pause(void);
vam_result vam_resume(void);
vam_result vam_next(void);
/* Restart the track if position > 3 s, else go to the previous track. */
vam_result vam_prev(void);
vam_result vam_seek(uint32_t position_ms);
vam_result vam_set_shuffle(int enabled);
vam_result vam_set_repeat(int32_t mode);  /* vam_repeat */
vam_result vam_set_volume(uint16_t volume);
/* Streaming quality, 96, 160 or 320 kbps (vam_config.bitrate at start).
 * Applies from the next track loaded. */
vam_result vam_set_bitrate(int32_t kbps);
vam_result vam_add_to_queue(const char *track_uri);

/* ------------------------------------------------------------------------ */
/* Library                                                                   */
/* ------------------------------------------------------------------------ */

/* Like/unlike a track, save/remove an album, follow/unfollow a playlist or
 * artist. Replies with VAM_EV_REQUEST_DONE, and on success also emits
 * VAM_EV_SAVED_CHANGED so every visible copy of `uri` updates. */
uint32_t vam_set_saved(const char *uri, int saved);

#ifdef __cplusplus
}
#endif

#endif /* VAM_BACKEND_H */
