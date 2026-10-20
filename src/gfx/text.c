#include "text.h"

#include <SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

/* Faces: primary regular/bold, plus two fallbacks for scripts the primary
 * lacks (e.g. Chinese/Japanese, then Korean). Fallbacks use fake bold. */
enum { FACE_REG, FACE_BOLD, FACE_FB_REG, FACE_FB_BOLD, FACE_FB2_REG, FACE_FB2_BOLD, FACE_COUNT };

static const int SIZES[] = { 14, 16, 19, 22, 24, 32, 44 };
#define NUM_SIZES ((int)(sizeof(SIZES) / sizeof(SIZES[0])))

typedef struct face {
    char path[512];
    bool ok;
    bool fake_bold;
    TTF_Font *f[NUM_SIZES];
} face;

static face g_faces[FACE_COUNT];
static bool g_ready;
static uint32_t g_frame;

#define CACHE_N 640
#define BUCKETS 1024        /* power of two */
typedef struct entry {
    uint32_t hash;
    char *str;
    int16_t si, weight;
    int max_w;
    SDL_Texture *tex;
    int w, h;
    uint32_t used;
    int16_t next;           /* bucket chain, -1 ends it */
} entry;
static entry g_cache[CACHE_N];
static int16_t g_bucket[BUCKETS];

/* Rendering a string (TTF + texture upload) costs milliseconds on the Vita,
 * far more for CJK text. Stop rendering new strings once a frame has spent
 * its budget (always allowing one) so a page fills in over a few frames
 * instead of stalling one. */
#define RENDERS_PER_FRAME 8
#define RENDER_BUDGET_MS 4.0
static int g_renders;
static Uint64 g_render_deadline;
static entry g_pending;     /* stand-in returned once the budget is spent */

/* Font files read into memory and shared by every size and weight.
 * Opening by path made FreeType stream glyphs from app0: with blocking
 * reads. Reading the 16 MB CJK font takes ~2 s on the Vita, so a worker
 * thread loads the files; until it finishes, fonts stream from disk, and
 * once it does they are reopened from memory between frames. */
#define FONT_FILES 4
static struct { char path[512]; void *data; size_t size; } g_files[FONT_FILES];
static int g_nfiles;
static SDL_Thread *g_preload;
static SDL_atomic_t g_preloaded;    /* worker finished writing g_files */
static bool g_reopened;

#define WRAP_N 16
#define WRAP_LINES 6
typedef struct wrap_entry {
    uint32_t hash;
    char *str;
    int si, weight, max_w, max_lines;
    int n;
    char *lines[WRAP_LINES];
    uint32_t used;
} wrap_entry;
static wrap_entry g_wrap[WRAP_N];

/* ------------------------------------------------------------------------ */

static bool file_exists(const char *p)
{
    SDL_RWops *rw = SDL_RWFromFile(p, "rb");
    if (!rw)
        return false;
    SDL_RWclose(rw);
    return true;
}

/* First existing path from a NULL-terminated candidate list. Entries that
 * start with '@' are bundled assets, '~' are in the writable data dir. */
static bool resolve(const char *const *cands, char *out, size_t n)
{
    char buf[512];
    for (int i = 0; cands[i]; i++) {
        const char *c = cands[i];
        if (c[0] == '@')
            c = platform_asset_path(c + 1, buf, sizeof(buf));
        else if (c[0] == '~') {
            snprintf(buf, sizeof(buf), "%s%s", platform_data_dir(), c + 1);
            c = buf;
        }
        if (file_exists(c)) {
            snprintf(out, n, "%s", c);
            return true;
        }
    }
    return false;
}

static const char *const REG_CANDS[] = {
    "~fonts/regular.ttf", "~fonts/regular.otf",
    "@fonts/regular.ttf", "@fonts/regular.otf",
#ifndef __vita__
    "C:/Windows/Fonts/segoeui.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
#endif
    NULL
};
static const char *const BOLD_CANDS[] = {
    "~fonts/bold.ttf", "~fonts/bold.otf",
    "@fonts/bold.ttf", "@fonts/bold.otf",
#ifndef __vita__
    "C:/Windows/Fonts/segoeuib.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
#endif
    NULL
};
static const char *const FB_CANDS[] = {
    "~fonts/fallback.ttf", "~fonts/fallback.otf", "~fonts/fallback.ttc",
    "@fonts/fallback.ttf", "@fonts/fallback.otf", "@fonts/fallback.ttc",
#ifndef __vita__
    "C:/Windows/Fonts/msjh.ttc",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/System/Library/Fonts/PingFang.ttc",
#endif
    NULL
};
static const char *const FB2_CANDS[] = {
    "~fonts/fallback2.ttf", "~fonts/fallback2.otf", "~fonts/fallback2.ttc",
    "@fonts/fallback2.ttf", "@fonts/fallback2.otf", "@fonts/fallback2.ttc",
#ifndef __vita__
    "C:/Windows/Fonts/malgun.ttf",
    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
#endif
    NULL
};

static int preload_worker(void *arg)
{
    (void)arg;
    platform_lower_thread_priority();
    for (int i = 0; i < g_nfiles; i++)
        g_files[i].data = SDL_LoadFile(g_files[i].path, &g_files[i].size);
    SDL_AtomicSet(&g_preloaded, 1);
    return 0;
}

/* Queue each distinct face file for loading into memory. */
static void start_preload(void)
{
    g_nfiles = 0;
    g_reopened = false;
    SDL_AtomicSet(&g_preloaded, 0);
    for (int i = 0; i < FACE_COUNT && g_nfiles < FONT_FILES; i++) {
        if (!g_faces[i].ok)
            continue;
        bool seen = false;
        for (int j = 0; j < g_nfiles; j++)
            seen = seen || strcmp(g_files[j].path, g_faces[i].path) == 0;
        if (!seen)
            snprintf(g_files[g_nfiles++].path, sizeof(g_files[0].path), "%s", g_faces[i].path);
    }
    g_preload = SDL_CreateThread(preload_worker, "font-preload", NULL);
    if (!g_preload)
        SDL_AtomicSet(&g_preloaded, 1); /* no thread: keep streaming from disk */
}

bool text_init(void)
{
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init: %s", TTF_GetError());
        return false;
    }
    memset(g_faces, 0, sizeof(g_faces));
    for (int i = 0; i < BUCKETS; i++)
        g_bucket[i] = -1;
    face *reg = &g_faces[FACE_REG], *bold = &g_faces[FACE_BOLD];
    face *fbr = &g_faces[FACE_FB_REG], *fbb = &g_faces[FACE_FB_BOLD];
    face *fb2r = &g_faces[FACE_FB2_REG], *fb2b = &g_faces[FACE_FB2_BOLD];

    reg->ok = resolve(REG_CANDS, reg->path, sizeof(reg->path));
    bold->ok = resolve(BOLD_CANDS, bold->path, sizeof(bold->path));
    fbr->ok = resolve(FB_CANDS, fbr->path, sizeof(fbr->path));
    fb2r->ok = resolve(FB2_CANDS, fb2r->path, sizeof(fb2r->path));

    if (!reg->ok && fbr->ok) {
        /* No Latin font, but a CJK one covers Latin too. */
        *reg = *fbr;
        fbr->ok = false;
    }
    if (!reg->ok) {
        SDL_Log("No font found. Put regular.ttf/bold.ttf in assets/fonts/.");
        return false;
    }
    if (!bold->ok) {
        *bold = *reg;
        bold->fake_bold = true;
    }
    if (fbr->ok) {
        *fbb = *fbr;
        fbb->fake_bold = true;
    }
    if (fb2r->ok) {
        *fb2b = *fb2r;
        fb2b->fake_bold = true;
    }
    SDL_Log("fonts: regular=%s bold=%s fallback=%s fallback2=%s", reg->path, bold->path,
            fbr->ok ? fbr->path : "(none)", fb2r->ok ? fb2r->path : "(none)");
    start_preload();
    g_ready = true;
    return true;
}

void text_shutdown(void)
{
    for (int i = 0; i < CACHE_N; i++) {
        if (g_cache[i].tex)
            SDL_DestroyTexture(g_cache[i].tex);
        free(g_cache[i].str);
    }
    memset(g_cache, 0, sizeof(g_cache));
    for (int i = 0; i < BUCKETS; i++)
        g_bucket[i] = -1;
    for (int i = 0; i < WRAP_N; i++) {
        free(g_wrap[i].str);
        for (int j = 0; j < g_wrap[i].n; j++)
            free(g_wrap[i].lines[j]);
    }
    memset(g_wrap, 0, sizeof(g_wrap));
    if (g_preload) {
        SDL_WaitThread(g_preload, NULL);
        g_preload = NULL;
    }
    /* Faces may share paths but each TTF_Font is a separate handle. */
    for (int i = 0; i < FACE_COUNT; i++)
        for (int s = 0; s < NUM_SIZES; s++)
            if (g_faces[i].f[s])
                TTF_CloseFont(g_faces[i].f[s]);
    memset(g_faces, 0, sizeof(g_faces));
    for (int i = 0; i < FONT_FILES; i++)
        SDL_free(g_files[i].data);
    memset(g_files, 0, sizeof(g_files));
    g_nfiles = 0;
    g_ready = false;
    TTF_Quit();
}

void text_begin_frame(void)
{
    g_frame++;
    if (!g_reopened && SDL_AtomicGet(&g_preloaded)) {
        /* Fonts opened while the files were loading stream from disk;
           reopen them from memory. Cached textures don't need the fonts. */
        g_reopened = true;
        for (int i = 0; i < FACE_COUNT; i++)
            for (int s = 0; s < NUM_SIZES; s++)
                if (g_faces[i].f[s]) {
                    TTF_CloseFont(g_faces[i].f[s]);
                    g_faces[i].f[s] = NULL;
                }
    }
    g_renders = 0;
    g_render_deadline =
        SDL_GetPerformanceCounter() + (Uint64)(RENDER_BUDGET_MS * SDL_GetPerformanceFrequency() / 1000.0);
}

int text_renders_this_frame(void) { return g_renders; }

static int size_index(int px)
{
    int best = 0;
    for (int i = 1; i < NUM_SIZES; i++)
        if (abs(SIZES[i] - px) < abs(SIZES[best] - px))
            best = i;
    return best;
}

/* The in-memory copy of path once preloaded, otherwise the file itself. */
static SDL_RWops *font_stream(const char *path)
{
    if (SDL_AtomicGet(&g_preloaded)) {
        for (int i = 0; i < g_nfiles; i++)
            if (g_files[i].data && strcmp(g_files[i].path, path) == 0)
                return SDL_RWFromConstMem(g_files[i].data, (int)g_files[i].size);
    }
    return SDL_RWFromFile(path, "rb");
}

static TTF_Font *font(int fi, int si)
{
    face *f = &g_faces[fi];
    if (!f->ok)
        return NULL;
    if (!f->f[si]) {
        SDL_RWops *rw = font_stream(f->path);
        f->f[si] = rw ? TTF_OpenFontRW(rw, 1, SIZES[si]) : NULL;
        if (!f->f[si]) {
            SDL_Log("TTF_OpenFont(%s): %s", f->path, TTF_GetError());
            f->ok = false;
            return NULL;
        }
        TTF_SetFontHinting(f->f[si], TTF_HINTING_LIGHT);
        if (f->fake_bold)
            TTF_SetFontStyle(f->f[si], TTF_STYLE_BOLD);
    }
    return f->f[si];
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    int n = 1;
    if (c >= 0xF0 && s[1] && s[2] && s[3]) {
        c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        n = 4;
    } else if (c >= 0xE0 && s[1] && s[2]) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if (c >= 0xC0 && s[1]) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    }
    *ps += n;
    return c;
}

/* A run is a maximal substring rendered with one font. */
typedef struct run { const char *start; int len; TTF_Font *f; } run;
#define MAX_RUNS 48

static int split_runs(const char *s, int si, int weight, run *runs)
{
    TTF_Font *prim = font(weight ? FACE_BOLD : FACE_REG, si);
    TTF_Font *fb = font(weight ? FACE_FB_BOLD : FACE_FB_REG, si);
    TTF_Font *fb2 = font(weight ? FACE_FB2_BOLD : FACE_FB2_REG, si);
    int n = 0;
    const char *p = s;
    while (*p) {
        const char *cs = p;
        uint32_t cp = utf8_next(&p);
        TTF_Font *f = prim;
        if (cp > 0x7F && cp <= 0xFFFF && !TTF_GlyphIsProvided(prim, (Uint16)cp)) {
            if (fb && TTF_GlyphIsProvided(fb, (Uint16)cp))
                f = fb;
            else if (fb2 && TTF_GlyphIsProvided(fb2, (Uint16)cp))
                f = fb2;
        }
        bool space = cp == ' ';
        if (n > 0 && (runs[n - 1].f == f || space)) {
            runs[n - 1].len += (int)(p - cs);
        } else if (n < MAX_RUNS) {
            runs[n].start = cs;
            runs[n].len = (int)(p - cs);
            runs[n].f = f;
            n++;
        } else {
            runs[n - 1].len += (int)(p - cs);
        }
    }
    return n;
}

static int measure(const char *s, int si, int weight)
{
    run runs[MAX_RUNS];
    int n = split_runs(s, si, weight, runs);
    int total = 0;
    char buf[1024];
    for (int i = 0; i < n; i++) {
        int len = runs[i].len < (int)sizeof(buf) - 1 ? runs[i].len : (int)sizeof(buf) - 1;
        memcpy(buf, runs[i].start, len);
        buf[len] = 0;
        int w = 0, h = 0;
        if (runs[i].f && TTF_SizeUTF8(runs[i].f, buf, &w, &h) == 0)
            total += w;
    }
    return total;
}

static SDL_Surface *render(const char *s, int si, int weight)
{
    run runs[MAX_RUNS];
    int n = split_runs(s, si, weight, runs);
    if (n == 0)
        return NULL;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *parts[MAX_RUNS];
    int asc[MAX_RUNS];
    int total_w = 0, max_asc = 0, max_desc = 0;
    char buf[1024];
    for (int i = 0; i < n; i++) {
        int len = runs[i].len < (int)sizeof(buf) - 1 ? runs[i].len : (int)sizeof(buf) - 1;
        memcpy(buf, runs[i].start, len);
        buf[len] = 0;
        parts[i] = runs[i].f ? TTF_RenderUTF8_Blended(runs[i].f, buf, white) : NULL;
        asc[i] = runs[i].f ? TTF_FontAscent(runs[i].f) : 0;
        if (parts[i]) {
            total_w += parts[i]->w;
            if (asc[i] > max_asc) max_asc = asc[i];
            int desc = parts[i]->h - asc[i];
            if (desc > max_desc) max_desc = desc;
        }
    }
    if (n == 1)
        return parts[0];
    SDL_Surface *out = NULL;
    if (total_w > 0)
        out = SDL_CreateRGBSurfaceWithFormat(0, total_w, max_asc + max_desc, 32, SDL_PIXELFORMAT_ARGB8888);
    int x = 0;
    for (int i = 0; i < n; i++) {
        if (!parts[i])
            continue;
        if (out) {
            SDL_SetSurfaceBlendMode(parts[i], SDL_BLENDMODE_NONE);
            SDL_Rect dst = { x, max_asc - asc[i], parts[i]->w, parts[i]->h };
            SDL_BlitSurface(parts[i], NULL, out, &dst);
            x += parts[i]->w;
        }
        SDL_FreeSurface(parts[i]);
    }
    return out;
}

/* Byte offsets of each code point boundary, for ellipsizing. */
static void ellipsize(const char *s, int si, int weight, int max_w, char *out, size_t outsz)
{
    static const char ELL[] = "\xE2\x80\xA6"; /* U+2026 */
    int offs[1024];
    int n = 0;
    const char *p = s;
    while (*p && n < 1023) {
        utf8_next(&p);
        offs[n++] = (int)(p - s);
    }
    int lo = 0, hi = n, best = 0;
    char tmp[1100];
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int bytes = mid ? offs[mid - 1] : 0;
        if (bytes > 1000) bytes = 1000;
        memcpy(tmp, s, bytes);
        /* Trim trailing spaces before the ellipsis. */
        while (bytes > 0 && tmp[bytes - 1] == ' ') bytes--;
        memcpy(tmp + bytes, ELL, sizeof(ELL));
        if (measure(tmp, si, weight) <= max_w) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    int bytes = best ? offs[best - 1] : 0;
    while (bytes > 0 && s[bytes - 1] == ' ') bytes--;
    if ((size_t)bytes + sizeof(ELL) > outsz)
        bytes = (int)(outsz - sizeof(ELL));
    memcpy(out, s, bytes);
    memcpy(out + bytes, ELL, sizeof(ELL));
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t bucket_of(uint32_t h, int si, int weight, int max_w)
{
    return (h ^ (uint32_t)si * 0x9E3779B1u ^ (uint32_t)weight * 0x85EBCA77u ^ (uint32_t)max_w * 0xC2B2AE3Du) &
           (BUCKETS - 1);
}

static void unlink_entry(int slot)
{
    entry *e = &g_cache[slot];
    int16_t *link = &g_bucket[bucket_of(e->hash, e->si, e->weight, e->max_w)];
    while (*link >= 0) {
        if (*link == slot) {
            *link = e->next;
            return;
        }
        link = &g_cache[*link].next;
    }
}

static entry *lookup(const char *s, int si, int weight, int max_w)
{
    uint32_t h = hash_str(s);
    uint32_t b = bucket_of(h, si, weight, max_w);
    for (int i = g_bucket[b]; i >= 0; i = g_cache[i].next) {
        entry *e = &g_cache[i];
        if (e->hash == h && e->si == si && e->weight == weight && e->max_w == max_w &&
            strcmp(e->str, s) == 0) {
            e->used = g_frame;
            return e;
        }
    }

    if (g_renders >= RENDERS_PER_FRAME || (g_renders > 0 && SDL_GetPerformanceCounter() > g_render_deadline)) {
        /* Over budget: report the width layout needs, draw nothing yet. */
        int w = measure(s, si, weight);
        memset(&g_pending, 0, sizeof(g_pending));
        g_pending.w = max_w > 0 && w > max_w ? max_w : w;
        return &g_pending;
    }
    g_renders++;

    int free_slot = -1, lru = 0;
    for (int i = 0; i < CACHE_N; i++) {
        entry *e = &g_cache[i];
        if (!e->str) {
            free_slot = i;
            break;
        }
        if (e->used < g_cache[lru].used)
            lru = i;
    }
    int slot = free_slot >= 0 ? free_slot : lru;
    entry *e = &g_cache[slot];
    if (e->str)
        unlink_entry(slot);
    gfx_destroy_texture_later(e->tex);
    free(e->str);
    memset(e, 0, sizeof(*e));

    char fitted[1100];
    const char *src = s;
    if (max_w > 0 && measure(s, si, weight) > max_w) {
        ellipsize(s, si, weight, max_w, fitted, sizeof(fitted));
        src = fitted;
    }
    SDL_Surface *surf = render(src, si, weight);
    if (surf) {
        e->tex = SDL_CreateTextureFromSurface(gfx_renderer(), surf);
        e->w = surf->w;
        e->h = surf->h;
        SDL_FreeSurface(surf);
        if (e->tex)
            SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
    }
    e->hash = h;
    e->str = SDL_strdup(s);
    e->si = (int16_t)si;
    e->weight = (int16_t)weight;
    e->max_w = max_w;
    e->used = g_frame;
    e->next = g_bucket[b];
    g_bucket[b] = (int16_t)slot;
    return e;
}

int text_line_height(int size)
{
    if (!g_ready)
        return size + 4;
    TTF_Font *f = font(FACE_REG, size_index(size));
    return f ? TTF_FontHeight(f) : size + 4;
}

int text_width(const char *s, int size, font_weight w, float max_w)
{
    if (!g_ready || !s || !*s)
        return 0;
    entry *e = lookup(s, size_index(size), w, max_w > 0 ? (int)max_w : 0);
    return e->w;
}

int text_draw(const char *s, float x, float y, int size, font_weight w, color c, float max_w)
{
    if (!g_ready || !s || !*s || c.a == 0)
        return 0;
    if (max_w > 0 && max_w < 4)
        return 0;
    entry *e = lookup(s, size_index(size), w, max_w > 0 ? (int)max_w : 0);
    if (!e->tex)
        return e->w; /* not rendered yet (budget) or blank */
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_FRect dst = { floorf(x + 0.5f), floorf(y + 0.5f), (float)e->w, (float)e->h };
    SDL_RenderCopyF(gfx_renderer(), e->tex, NULL, &dst);
    return e->w;
}

int text_draw_aligned(const char *s, float x, float y, int size, font_weight w, color c,
                      float max_w, text_align align)
{
    if (align == ALIGN_LEFT)
        return text_draw(s, x, y, size, w, c, max_w);
    int tw = text_width(s, size, w, max_w);
    if (align == ALIGN_CENTER)
        return text_draw(s, x - tw / 2.0f, y, size, w, c, max_w);
    return text_draw(s, x - tw, y, size, w, c, max_w);
}

int text_draw_vc(const char *s, float x, float cy, int size, font_weight w, color c, float max_w)
{
    return text_draw(s, x, cy - text_line_height(size) / 2.0f, size, w, c, max_w);
}

static wrap_entry *wrap_layout(const char *s, int size, font_weight w, int max_w, int max_lines)
{
    int si = size_index(size);
    uint32_t h = hash_str(s);
    int lru = 0;
    for (int i = 0; i < WRAP_N; i++) {
        wrap_entry *e = &g_wrap[i];
        if (e->str && e->hash == h && e->si == si && e->weight == (int)w && e->max_w == max_w &&
            e->max_lines == max_lines && strcmp(e->str, s) == 0) {
            e->used = g_frame;
            return e;
        }
        if (!e->str || (g_wrap[lru].str && e->used < g_wrap[lru].used))
            lru = i;
    }
    wrap_entry *e = &g_wrap[lru];
    free(e->str);
    for (int j = 0; j < e->n; j++)
        free(e->lines[j]);
    memset(e, 0, sizeof(*e));
    e->hash = h;
    e->str = SDL_strdup(s);
    e->si = si;
    e->weight = w;
    e->max_w = max_w;
    e->max_lines = max_lines;
    e->used = g_frame;
    if (max_lines > WRAP_LINES)
        max_lines = WRAP_LINES;

    const char *p = s;
    char line[1024];
    while (*p && e->n < max_lines) {
        while (*p == ' ') p++;
        if (e->n == max_lines - 1) {
            /* Last line takes the rest; drawing ellipsizes it. */
            e->lines[e->n++] = SDL_strdup(p);
            break;
        }
        int len = 0, last_break = -1;
        const char *q = p;
        while (*q && *q != '\n') {
            const char *cs = q;
            uint32_t cp = utf8_next(&q);
            int cl = (int)(q - cs);
            if (len + cl >= (int)sizeof(line) - 1)
                break;
            memcpy(line + len, cs, cl);
            line[len + cl] = 0;
            if (measure(line, si, w) > max_w && len > 0) {
                q = cs;
                break;
            }
            len += cl;
            /* Break after spaces, or anywhere between CJK characters. */
            if (cp == ' ' || cp >= 0x2E80)
                last_break = len;
        }
        if (*q && *q != '\n' && last_break > 0)
            len = last_break;
        line[len] = 0;
        e->lines[e->n++] = SDL_strdup(line);
        p += len;
        if (*p == '\n') p++;
    }
    return e;
}

float text_draw_wrapped(const char *s, float x, float y, int size, font_weight w, color c,
                        float max_w, int max_lines)
{
    if (!g_ready || !s || !*s)
        return 0;
    wrap_entry *e = wrap_layout(s, size, w, (int)max_w, max_lines);
    int lh = text_line_height(size);
    for (int i = 0; i < e->n; i++)
        text_draw(e->lines[i], x, y + i * lh, size, w, c, max_w);
    return (float)(e->n * lh);
}
