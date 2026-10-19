#include "images.h"

#include "../platform/platform.h"

#include <SDL_image.h>
#include <stdlib.h>
#include <string.h>

#define IMG_N 96            /* cached textures (~350 KB each at 300x300)  */
#define MAX_IN_FLIGHT 6
#define UPLOADS_PER_FRAME 2 /* decoded surfaces turned into textures      */
#define UPLOAD_BUDGET_MS 3.0
#define MAX_DIM 320         /* larger images are box-downscaled           */
#define JOB_N (IMG_N * 2)   /* decode jobs outstanding, stale ones included */

typedef enum { IS_EMPTY, IS_WANTED, IS_LOADING, IS_DOWNLOADED, IS_DECODING, IS_READY, IS_FAILED } img_state;

typedef struct img {
    img_state state;
    char *url;
    uint32_t req;
    uint32_t gen;           /* matches the decode job in flight           */
    uint8_t *data;
    uint32_t size;
    SDL_Texture *tex;
    color avg;
    uint32_t used;
} img;

/* JPEG/PNG decoding takes tens of ms on the Vita, so it runs on a worker
 * thread; only the texture upload stays on the render thread. */
typedef struct job {
    int slot;
    uint32_t gen;
    uint8_t *data;
    uint32_t size;
    SDL_Surface *surf;
    color avg;
} job;

static img g_imgs[IMG_N];
static uint32_t g_frame;
static int g_in_flight;

static SDL_Thread *g_thread;
static SDL_mutex *g_mu;
static SDL_cond *g_cv;
static bool g_quit;
static job g_todo[JOB_N], g_done[JOB_N];
static int g_todo_n, g_done_n;
static int g_jobs;          /* submitted, result not yet taken (render thread only) */
static uint32_t g_gen;
static int g_uploads;

static SDL_Surface *decode_surface(const uint8_t *data, uint32_t size, color *avg);

static int decode_worker(void *arg)
{
    (void)arg;
    platform_lower_thread_priority();
    SDL_LockMutex(g_mu);
    for (;;) {
        while (!g_quit && g_todo_n == 0)
            SDL_CondWait(g_cv, g_mu);
        if (g_quit)
            break;
        job j = g_todo[0];
        memmove(g_todo, g_todo + 1, (size_t)(--g_todo_n) * sizeof(job));
        SDL_UnlockMutex(g_mu);

        j.surf = decode_surface(j.data, j.size, &j.avg);
        free(j.data);
        j.data = NULL;

        SDL_LockMutex(g_mu);
        g_done[g_done_n++] = j; /* g_jobs <= JOB_N keeps this in bounds */
    }
    SDL_UnlockMutex(g_mu);
    return 0;
}

void images_init(void)
{
    memset(g_imgs, 0, sizeof(g_imgs));
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    g_quit = false;
    g_mu = SDL_CreateMutex();
    g_cv = SDL_CreateCond();
    if (g_mu && g_cv)
        g_thread = SDL_CreateThreadWithStackSize(decode_worker, "img-decode", 256 * 1024, NULL);
    if (!g_thread)
        SDL_Log("image decode thread: %s (decoding inline)", SDL_GetError());
}

static void clear(img *m)
{
    if (m->state == IS_LOADING) {
        vam_cancel(m->req);
        g_in_flight--;
    }
    /* An IS_DECODING job finishes on its own; its result won't match gen. */
    gfx_destroy_texture_later(m->tex);
    free(m->url);
    free(m->data);
    memset(m, 0, sizeof(*m));
}

void images_shutdown(void)
{
    if (g_thread) {
        SDL_LockMutex(g_mu);
        g_quit = true;
        SDL_CondSignal(g_cv);
        SDL_UnlockMutex(g_mu);
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
    }
    for (int i = 0; i < g_todo_n; i++)
        free(g_todo[i].data);
    for (int i = 0; i < g_done_n; i++)
        SDL_FreeSurface(g_done[i].surf);
    g_todo_n = g_done_n = g_jobs = 0;
    if (g_cv)
        SDL_DestroyCond(g_cv);
    if (g_mu)
        SDL_DestroyMutex(g_mu);
    g_cv = NULL;
    g_mu = NULL;
    for (int i = 0; i < IMG_N; i++)
        clear(&g_imgs[i]);
    g_in_flight = 0;
    IMG_Quit();
}

static SDL_Surface *downscale_half(SDL_Surface *s)
{
    int w = s->w / 2, h = s->h / 2;
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!d)
        return s;
    for (int y = 0; y < h; y++) {
        const Uint32 *r0 = (const Uint32 *)((const Uint8 *)s->pixels + (y * 2) * s->pitch);
        const Uint32 *r1 = (const Uint32 *)((const Uint8 *)s->pixels + (y * 2 + 1) * s->pitch);
        Uint32 *o = (Uint32 *)((Uint8 *)d->pixels + y * d->pitch);
        for (int x = 0; x < w; x++) {
            Uint32 p[4] = { r0[x * 2], r0[x * 2 + 1], r1[x * 2], r1[x * 2 + 1] };
            Uint32 out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                Uint32 sum = 0;
                for (int k = 0; k < 4; k++)
                    sum += (p[k] >> sh) & 0xFF;
                out |= ((sum / 4) & 0xFF) << sh;
            }
            o[x] = out;
        }
    }
    SDL_FreeSurface(s);
    return d;
}

/* Average colour, pushed toward a dark, saturated tone for backgrounds. */
static color dominant(SDL_Surface *s)
{
    uint64_t r = 0, g = 0, b = 0, n = 0;
    int step = s->w / 24 > 0 ? s->w / 24 : 1;
    for (int y = 0; y < s->h; y += step) {
        const Uint32 *row = (const Uint32 *)((const Uint8 *)s->pixels + y * s->pitch);
        for (int x = 0; x < s->w; x += step) {
            Uint32 p = row[x];
            Uint8 pr = (p >> 16) & 0xFF, pg = (p >> 8) & 0xFF, pb = p & 0xFF;
            int mx = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
            int mn = pr < pg ? (pr < pb ? pr : pb) : (pg < pb ? pg : pb);
            /* Weight colourful pixels higher so greys don't wash it out. */
            int wgt = 1 + (mx - mn) / 16;
            r += pr * wgt; g += pg * wgt; b += pb * wgt; n += wgt;
        }
    }
    if (n == 0)
        return rgb(0x404040);
    float fr = (float)r / n, fg = (float)g / n, fb = (float)b / n;
    float mx = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
    float target = 120.0f;
    float k = mx > 1 ? target / mx : 1;
    if (k > 1.6f) k = 1.6f;
    color c = { (Uint8)(fr * k), (Uint8)(fg * k), (Uint8)(fb * k), 255 };
    return c;
}

/* Thread-safe: touches no renderer state. */
static SDL_Surface *decode_surface(const uint8_t *data, uint32_t size, color *avg)
{
    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    SDL_Surface *raw = rw ? IMG_Load_RW(rw, 1) : NULL;
    if (!raw)
        return NULL;
    SDL_Surface *s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    if (!s)
        return NULL;
    while (s->w > MAX_DIM * 2 - 40 && s->h > MAX_DIM * 2 - 40)
        s = downscale_half(s);
    *avg = dominant(s);
    return s;
}

static void upload(img *m, SDL_Surface *s, color avg)
{
    if (!s) {
        m->state = IS_FAILED;
        return;
    }
    m->avg = avg;
    g_uploads++;
    m->tex = SDL_CreateTextureFromSurface(gfx_renderer(), s);
    SDL_FreeSurface(s);
    if (!m->tex) {
        m->state = IS_FAILED;
        return;
    }
    SDL_SetTextureScaleMode(m->tex, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(m->tex, SDL_BLENDMODE_BLEND);
    m->state = IS_READY;
}

int images_uploads_this_frame(void) { return g_uploads; }

void images_update(void)
{
    g_frame++;
    g_uploads = 0;

    /* Turn finished decodes into textures: a couple per frame at most, and
     * only one when it was slow (texture creation syncs with the GPU on the
     * Vita). */
    Uint64 start = SDL_GetPerformanceCounter();
    Uint64 budget = (Uint64)(UPLOAD_BUDGET_MS * SDL_GetPerformanceFrequency() / 1000.0);
    for (int n = 0; g_thread && n < UPLOADS_PER_FRAME; n++) {
        if (n > 0 && SDL_GetPerformanceCounter() - start > budget)
            break;
        SDL_LockMutex(g_mu);
        bool have = g_done_n > 0;
        job j;
        if (have) {
            j = g_done[0];
            memmove(g_done, g_done + 1, (size_t)(--g_done_n) * sizeof(job));
        }
        SDL_UnlockMutex(g_mu);
        if (!have)
            break;
        g_jobs--;
        img *m = &g_imgs[j.slot];
        if (m->state == IS_DECODING && m->gen == j.gen)
            upload(m, j.surf, j.avg);
        else
            SDL_FreeSurface(j.surf);
    }

    /* Hand new downloads to the worker (or decode one inline without it). */
    bool queued = false;
    for (int i = 0; i < IMG_N; i++) {
        img *m = &g_imgs[i];
        if (m->state != IS_DOWNLOADED)
            continue;
        if (!g_thread) {
            color avg = { 0, 0, 0, 255 };
            SDL_Surface *s = decode_surface(m->data, m->size, &avg);
            free(m->data);
            m->data = NULL;
            m->size = 0;
            upload(m, s, avg);
            break;
        }
        if (g_jobs >= JOB_N)
            break;
        job j = { i, ++g_gen, m->data, m->size, NULL, { 0, 0, 0, 255 } };
        m->data = NULL;
        m->size = 0;
        m->gen = j.gen;
        m->state = IS_DECODING;
        g_jobs++;
        SDL_LockMutex(g_mu);
        g_todo[g_todo_n++] = j;
        SDL_UnlockMutex(g_mu);
        queued = true;
    }
    if (queued)
        SDL_CondSignal(g_cv);
    /* Issue requests for the most recently wanted images first. */
    while (g_in_flight < MAX_IN_FLIGHT) {
        img *best = NULL;
        for (int i = 0; i < IMG_N; i++) {
            img *m = &g_imgs[i];
            if (m->state == IS_WANTED && (!best || m->used > best->used))
                best = m;
        }
        if (!best)
            break;
        /* Skip things that scrolled away a while ago. */
        if (g_frame - best->used > 30) {
            clear(best);
            continue;
        }
        best->req = vam_request_image(best->url);
        if (!best->req) {
            best->state = IS_FAILED;
            continue;
        }
        best->state = IS_LOADING;
        g_in_flight++;
    }
}

SDL_Texture *images_get(const char *url, color *avg)
{
    if (!url || !*url)
        return NULL;
    img *slot = NULL, *lru = NULL;
    for (int i = 0; i < IMG_N; i++) {
        img *m = &g_imgs[i];
        if (m->state == IS_EMPTY) {
            if (!slot) slot = m;
            continue;
        }
        if (strcmp(m->url, url) == 0) {
            m->used = g_frame;
            if (m->state == IS_READY) {
                if (avg) *avg = m->avg;
                return m->tex;
            }
            return NULL;
        }
        /* Never evict art drawn in the last few frames: it would blink out
           and reload while still on screen. */
        if (m->state != IS_LOADING && m->state != IS_DECODING && g_frame - m->used > 3 &&
            (!lru || m->used < lru->used))
            lru = m;
    }
    if (!slot) {
        if (!lru)
            return NULL;
        clear(lru);
        slot = lru;
    }
    slot->url = SDL_strdup(url);
    slot->state = IS_WANTED;
    slot->used = g_frame;
    return NULL;
}

bool images_on_event(const vam_event *ev)
{
    if (ev->type != VAM_EV_IMAGE)
        return false;
    for (int i = 0; i < IMG_N; i++) {
        img *m = &g_imgs[i];
        if (m->state != IS_LOADING || m->req != ev->request_id)
            continue;
        g_in_flight--;
        if (ev->result == VAM_OK && ev->data && ev->data_size > 0) {
            m->data = malloc(ev->data_size);
            if (m->data) {
                memcpy(m->data, ev->data, ev->data_size);
                m->size = ev->data_size;
                m->state = IS_DOWNLOADED;
                return true;
            }
        }
        m->state = IS_FAILED;
        return true;
    }
    return true; /* reply for a request we already dropped */
}
