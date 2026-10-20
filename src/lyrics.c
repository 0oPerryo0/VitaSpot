#include "lyrics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "gfx/text.h"
#include "theme.h"

/*
 * Performance notes. The Vita renders a new string in milliseconds (CJK far
 * more), so the column only ever costs:
 *   - one wrap of every line when it is first shown (spread over frames),
 *   - one cached texture per visible row, tinted per state: every line is
 *     bold, so moving the highlight changes a colour, not a texture,
 *   - an O(1) check of the current line per frame (binary search on a seek).
 * Nothing runs while the column is hidden or the screen is off.
 */

#define LY_SIZE FS_L
#define LY_WEIGHT FW_BOLD
#define LINE_GAP 12.0f        /* px between lyric lines                  */
#define FOLLOW_AFTER 3.0f     /* s after a manual scroll before following */
#define LAYOUT_BUDGET_MS 2.0  /* wrapping time per frame                  */
#define EDGE_FADE 40.0f       /* px over which rows fade at the edges     */
#define PRERENDER_LINES 3     /* lines below the column rendered early    */
#define MAX_LINE_BYTES 512    /* the backend cuts longer lines            */

typedef enum ly_state {
    LY_NONE,     /* nothing to show for this track: no lyrics or not a song */
    LY_WANT,     /* fetch once the screen is on                            */
    LY_LOADING,
    LY_READY
} ly_state;

typedef struct lline {
    uint32_t start;   /* ms, synced lyrics only          */
    uint32_t text;    /* offset into L.text              */
    int row0, nrows;  /* rows in L.row_off once laid out */
    float y, h;       /* in column coordinates           */
} lline;

static struct {
    ly_state state;
    char uri[VAM_URI_MAX];
    uint32_t req;
    bool synced;
    int n;
    lline *lines;
    char *text;
    /* Layout: every line wrapped to wrap_w, rows NUL-separated in rows. */
    int wrap_w;
    int laid;
    char *rows;
    size_t rows_len, rows_cap;
    uint32_t *row_off;
    int nrows, row_cap;
    float content_h;
    /* Playback and scrolling */
    int cur;          /* current line, -1 before the first */
    scroller scroll;
    float user_at;    /* A.time of the last manual scroll   */
    float drawn_at;   /* A.time the column was last drawn   */
    bool need_jump;   /* place the current line without scrolling to it */
} L;

static void clear(void)
{
    if (L.req)
        vam_cancel(L.req);
    free(L.lines);
    free(L.text);
    free(L.rows);
    free(L.row_off);
    memset(&L, 0, sizeof(L));
    L.cur = -1;
    L.user_at = L.drawn_at = -1000;
}

void lyrics_reset(void) { clear(); }

bool lyrics_available(void) { return L.state == LY_READY; }

void lyrics_frame(void)
{
    const char *uri = A.player.has_track ? A.player.track.uri : "";
    if (strcmp(uri, L.uri) != 0) {
        clear();
        snprintf(L.uri, sizeof(L.uri), "%s", uri);
        L.state = strncmp(uri, "spotify:track:", 14) == 0 ? LY_WANT : LY_NONE;
    }
    /* With the screen off nobody can see lyrics: fetch on wake instead. */
    if (L.state == LY_WANT && app_drawing() && A.auth.status == VAM_AUTH_LOGGED_IN) {
        L.req = vam_request_lyrics(L.uri);
        L.state = L.req ? LY_LOADING : LY_NONE;
    }
}

void lyrics_on_event(const vam_event *ev)
{
    if (!ev->request_id || ev->request_id != L.req)
        return;
    L.req = 0;
    const vam_lyrics *ly = ev->lyrics;
    L.state = LY_NONE;
    if (ev->result != VAM_OK || !ly || !ly->line_count || !ly->lines || !ly->text || !ly->text_size)
        return;
    L.text = malloc(ly->text_size);
    L.lines = calloc(ly->line_count, sizeof(*L.lines));
    if (!L.text || !L.lines) {
        free(L.text);
        free(L.lines);
        L.text = NULL;
        L.lines = NULL;
        return;
    }
    memcpy(L.text, ly->text, ly->text_size);
    L.text[ly->text_size - 1] = 0;
    for (uint32_t i = 0; i < ly->line_count; i++) {
        L.lines[i].start = ly->lines[i].start_ms;
        L.lines[i].text = ly->lines[i].text_offset < ly->text_size ? ly->lines[i].text_offset
                                                                     : ly->text_size - 1;
    }
    L.n = (int)ly->line_count;
    L.synced = ly->synced != 0;
    L.state = LY_READY;
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

static bool push_row(const char *s, size_t len)
{
    if (L.nrows == L.row_cap) {
        int cap = L.row_cap ? L.row_cap * 2 : 128;
        uint32_t *p = realloc(L.row_off, cap * sizeof(*p));
        if (!p)
            return false;
        L.row_off = p;
        L.row_cap = cap;
    }
    if (L.rows_len + len + 1 > L.rows_cap) {
        size_t cap = L.rows_cap ? L.rows_cap * 2 : 4096;
        while (cap < L.rows_len + len + 1)
            cap *= 2;
        char *p = realloc(L.rows, cap);
        if (!p)
            return false;
        L.rows = p;
        L.rows_cap = cap;
    }
    L.row_off[L.nrows++] = (uint32_t)L.rows_len;
    memcpy(L.rows + L.rows_len, s, len);
    L.rows_len += len;
    L.rows[L.rows_len++] = 0;
    return true;
}

static uint32_t utf8_decode(const unsigned char *s, int *len)
{
    uint32_t c = s[0];
    if (c >= 0xF0 && s[1] && s[2] && s[3]) {
        *len = 4;
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    if (c >= 0xE0 && s[1] && s[2]) {
        *len = 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if (c >= 0xC0 && s[1]) {
        *len = 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    *len = 1;
    return c;
}

static bool is_cjk(uint32_t cp) { return cp >= 0x2E80; }

/* Rows of one line no wider than w. Most lines fit and cost one measure;
   longer ones find each row's end by binary search, then step back to a
   space or a CJK character so words stay whole. */
static void wrap_line(lline *ln, int w)
{
    const char *s = L.text + ln->text;
    ln->row0 = L.nrows;
    ln->nrows = 0;
    if (!*s)
        return;
    if (text_measure(s, LY_SIZE, LY_WEIGHT) <= w) {
        if (push_row(s, strlen(s)))
            ln->nrows = 1;
        return;
    }
    static int offs[MAX_LINE_BYTES + 1];   /* byte end of code point i */
    static uint32_t cps[MAX_LINE_BYTES + 1];
    char buf[MAX_LINE_BYTES + 8];
    const char *p = s;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        int m = 0, b = 0;
        while (p[b] && m < MAX_LINE_BYTES) {
            int cl;
            cps[m] = utf8_decode((const unsigned char *)p + b, &cl);
            b += cl;
            offs[m++] = b;
        }
        /* Longest prefix of k code points that fits; at least one. */
        int lo = 1, hi = m, k = 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            memcpy(buf, p, offs[mid - 1]);
            buf[offs[mid - 1]] = 0;
            if (text_measure(buf, LY_SIZE, LY_WEIGHT) <= w) {
                k = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        int j = k;
        if (k < m) {
            /* Break between code points j-1 and j. */
            for (j = k; j > 0; j--)
                if (cps[j] == ' ' || cps[j - 1] == ' ' || is_cjk(cps[j]) || is_cjk(cps[j - 1]))
                    break;
            if (j == 0)
                j = k;
        }
        int len = offs[j - 1];
        int keep = len;
        while (keep > 0 && p[keep - 1] == ' ')
            keep--;
        if (!push_row(p, (size_t)keep))
            return;
        ln->nrows++;
        p += len;
    }
}

/* Lays out lines until the frame's budget is spent. True when all are. */
static bool layout_step(int w)
{
    if (!text_fonts_ready())
        return false; /* CJK would measure with the wrong glyphs */
    if (w != L.wrap_w) {
        L.wrap_w = w;
        L.laid = 0;
        L.nrows = 0;
        L.rows_len = 0;
        L.content_h = 0;
    }
    if (L.laid == L.n)
        return true;
    Uint64 deadline =
        SDL_GetPerformanceCounter() + (Uint64)(LAYOUT_BUDGET_MS * SDL_GetPerformanceFrequency() / 1000.0);
    float lh = (float)text_line_height(LY_SIZE);
    do {
        lline *ln = &L.lines[L.laid++];
        wrap_line(ln, w);
        /* Empty lines are instrumental gaps: half a row of space. */
        ln->h = ln->nrows ? ln->nrows * lh : lh * 0.5f;
        ln->y = L.content_h;
        L.content_h += ln->h + LINE_GAP;
    } while (L.laid < L.n && SDL_GetPerformanceCounter() < deadline);
    return L.laid == L.n;
}

/* ------------------------------------------------------------------------ */
/* Playback position                                                         */
/* ------------------------------------------------------------------------ */

/* Last line that has started at pos, or -1. */
static int line_at_time(uint32_t pos)
{
    int lo = 0, hi = L.n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (L.lines[mid].start <= pos) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

static void update_cur(uint32_t pos)
{
    int c = L.cur;
    bool started = c < 0 || L.lines[c].start <= pos;
    bool next_pending = c + 1 >= L.n || L.lines[c + 1].start > pos;
    if (started && next_pending)
        return;
    /* Usually the next line; a seek can land anywhere. */
    if (started && (c + 2 >= L.n || L.lines[c + 2].start > pos))
        L.cur = c + 1;
    else
        L.cur = line_at_time(pos);
}

/* Last line starting at or above column y, or -1. With exact, -1 also when
   y is below that line and its gap. */
static int line_at_y(float y, bool exact)
{
    int lo = 0, hi = L.n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (L.lines[mid].y <= y) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (exact && best >= 0 && y > L.lines[best].y + L.lines[best].h + LINE_GAP)
        return -1;
    return best;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------ */

void lyrics_draw(rectf area, float alpha)
{
    if (L.state != LY_READY)
        return;
    /* Not drawn last frame: just shown, Now Playing reopened or the screen
       woke. Put the current line in place instead of scrolling past the
       lines that played meanwhile. */
    if (A.time - L.drawn_at > 0.25f)
        L.need_jump = true;
    L.drawn_at = A.time;

    if (!layout_step((int)area.w)) {
        gfx_spinner(area.x + area.w / 2, area.y + area.h / 3, 14, A.time, with_alpha(rgb(C_SUBTEXT), alpha));
        return;
    }
    if (L.synced)
        update_cur(app_position_ms());

    scroller *s = &L.scroll;
    float anchor = area.h * 0.33f;
    /* Synced: room below the last line so it can reach the anchor. */
    float content = L.synced ? L.content_h + area.h - anchor : L.content_h + 24;
    float prev = s->pos;
    bool had_target = s->has_target;

    ui_push_clip(area);
    gfx_push_clip(area);
    ui_scroll_begin(s, area, content, AXIS_Y, true);
    if (!had_target && s->pos != prev && !L.need_jump)
        L.user_at = A.time;

    if (L.synced) {
        float target = 0;
        if (L.cur >= 0)
            target = L.lines[L.cur].y + L.lines[L.cur].h / 2 - anchor;
        target = target < 0 ? 0 : target > s->max ? s->max : target;
        if (L.need_jump) {
            s->pos = target;
            s->vel = 0;
            s->has_target = false;
            L.user_at = -1000;
        } else if (A.time - L.user_at > FOLLOW_AFTER && s->vel == 0 && !ui_pointer_pressed_in(area) &&
                   fabsf(s->pos - target) > 0.5f && (!s->has_target || s->target != target)) {
            ui_scroll_to(s, target);
        }
    }
    L.need_jump = false;

    /* Tap a line to play from it. */
    ui_res res = ui_item(ui_id("np.lyrics"), area, UI_F_NOFOCUS);
    if (res.clicked && L.synced) {
        int i = line_at_y(ui_pointer_y() - area.y + s->pos, true);
        if (i >= 0 && L.lines[i].nrows > 0) {
            vam_seek(L.lines[i].start);
            L.cur = i;
            L.user_at = -1000;
        }
    }

    float lh = (float)text_line_height(LY_SIZE);
    float bottom = s->pos + area.h;
    int i = line_at_y(s->pos, false);
    if (i < 0)
        i = 0;
    int ahead = 0;
    color white = rgb(C_TEXT);
    /* Edges fade only where there is more to scroll to. */
    float fade_top = EDGE_FADE - fminf(EDGE_FADE, fmaxf(0, s->pos));
    float fade_bottom = EDGE_FADE - fminf(EDGE_FADE, fmaxf(0, s->max - s->pos));
    for (; i < L.n; i++) {
        lline *ln = &L.lines[i];
        if (ln->y >= bottom) {
            /* Below the column: render the next few rows early, inside
               text.c's per-frame budget, so they're ready when reached. */
            if (++ahead > PRERENDER_LINES)
                break;
            for (int r = 0; r < ln->nrows; r++)
                text_width(L.rows + L.row_off[ln->row0 + r], LY_SIZE, LY_WEIGHT, 0);
            continue;
        }
        if (ln->y + ln->h < s->pos)
            continue;
        float a = 0.9f;
        if (L.synced)
            a = i == L.cur ? 1.0f : i < L.cur ? 0.6f : 0.38f;
        for (int r = 0; r < ln->nrows; r++) {
            float y = area.y + ln->y - s->pos + r * lh;
            float mid = y + lh / 2;
            float edge = fminf(mid - area.y + fade_top, area.y + area.h - mid + fade_bottom) / EDGE_FADE;
            edge = edge < 0 ? 0 : edge > 1 ? 1 : edge;
            text_draw(L.rows + L.row_off[ln->row0 + r], area.x, y, LY_SIZE, LY_WEIGHT,
                      with_alpha(white, a * edge * alpha), 0);
        }
    }

    ui_scroll_end();
    gfx_pop_clip();
    ui_pop_clip();
}
