#include "ui.h"

#include <math.h>
#include <string.h>

#include "../theme.h"

#define MAX_FOC 2048
#define CLIP_STACK 8
#define SCROLL_STACK 4
#define DRAG_SLOP 10.0f
#define LONG_PRESS_MS 520
#define REPEAT_DELAY_MS 340
#define REPEAT_RATE_MS 85
#define COMBO_WINDOW_MS 150

typedef struct scroll_ctx {
    scroller *s;
    rectf vp;
    axis ax;
} scroll_ctx;

typedef struct focusable {
    uint32_t id;
    rectf r;
    unsigned flags;
    int nsc;
    scroll_ctx sc[SCROLL_STACK];
    float sc_pos[SCROLL_STACK];
} focusable;

static struct {
    /* pointer */
    bool down, ev_press, ev_release;
    bool pressed, released;
    float px, py, press_x, press_y, rel_x, rel_y;
    uint64_t press_time;
    int drag_axis;            /* 0, AXIS_X, AXIS_Y */
    bool long_fired;
    float dx, dy, acc_dx, acc_dy;
    float vx, vy;
    float wheel_x, wheel_y, acc_wheel_x, acc_wheel_y;
    SDL_FingerID finger;
    bool finger_active;

    /* deferred hits: decided in frame N, delivered in frame N+1 */
    uint32_t hot_cur, hot_prev;
    uint32_t click_cur, click_pending;
    uint32_t menu_cur, menu_pending;
    bool long_this_frame;
    int rel_drag_axis;
    bool rel_long_fired;

    /* buttons */
    bool held[BTN_COUNT];
    bool ev_pressed[BTN_COUNT];
    bool pressed_b[BTN_COUNT];
    uint64_t hold_since[BTN_COUNT], last_repeat[BTN_COUNT];
    float stick_x, stick_y;

    /* combo */
    btn combo[2];
    bool deferred[BTN_COUNT];
    uint64_t deferred_at[BTN_COUNT];
    bool combo_fired;
    bool capturing, captured;
    bool cap_ignore[BTN_COUNT]; /* held when capture began (e.g. Cross on "Change") */
    btn cap[2];

    /* focus */
    uint32_t focus;
    bool focus_visible;
    int slide;
    focusable prev[MAX_FOC], cur[MAX_FOC];
    int prev_n, cur_n;

    rectf clip[CLIP_STACK];
    int clip_n;
    scroll_ctx sstack[SCROLL_STACK];
    int sn;

    bool enabled, text_mode;
    float dt;
    uint64_t now;
} g;

void ui_init(void)
{
    memset(&g, 0, sizeof(g));
    g.enabled = true;
    g.combo[0] = g.combo[1] = BTN_COUNT;
}

void ui_set_combo(btn a, btn b)
{
    g.combo[0] = a;
    g.combo[1] = b;
    memset(g.deferred, 0, sizeof(g.deferred));
}

bool ui_combo_fired(void) { return g.combo_fired; }

void ui_capture_combo(bool on)
{
    g.capturing = on;
    g.captured = false;
    memcpy(g.cap_ignore, g.held, sizeof(g.cap_ignore));
}

bool ui_capturing_combo(void) { return g.capturing; }

bool ui_captured_combo(btn *a, btn *b)
{
    if (!g.captured)
        return false;
    g.captured = false;
    *a = g.cap[0];
    *b = g.cap[1];
    return true;
}

static void apply_combo(void)
{
    g.combo_fired = false;
    if (g.capturing) {
        int held[2] = { -1, -1 }, n = 0;
        for (int b = 0; b < BTN_COUNT; b++) {
            g.pressed_b[b] = false;
            if (!g.held[b])
                g.cap_ignore[b] = false;
            else if (!g.cap_ignore[b] && n < 2)
                held[n++] = b;
        }
        if (n == 2) {
            g.cap[0] = (btn)held[0];
            g.cap[1] = (btn)held[1];
            g.captured = true;
            g.capturing = false;
        }
        return;
    }
    btn a = g.combo[0], b = g.combo[1];
    if (a >= BTN_COUNT || b >= BTN_COUNT)
        return;
    const btn pair[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        btn x = pair[i];
        if (g.pressed_b[x]) {
            g.pressed_b[x] = false;
            if (!g.deferred[x]) {
                g.deferred[x] = true;
                g.deferred_at[x] = g.now;
            }
        }
    }
    if (g.held[a] && g.held[b] && (g.deferred[a] || g.deferred[b])) {
        g.deferred[a] = g.deferred[b] = false;
        g.combo_fired = true;
        return;
    }
    /* No partner in time (or already let go): act as a single press. */
    for (int i = 0; i < 2; i++) {
        btn x = pair[i];
        if (g.deferred[x] && (!g.held[x] || g.now - g.deferred_at[x] >= COMBO_WINDOW_MS)) {
            g.deferred[x] = false;
            g.pressed_b[x] = true;
        }
    }
}

uint64_t ui_ticks(void) { return g.now; }

uint32_t ui_id(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h ? h : 1;
}

uint32_t ui_idx(uint32_t base, uint32_t n)
{
    uint32_t h = base ^ (n + 0x9E3779B9u + (base << 6) + (base >> 2));
    h *= 2654435761u;
    return h ? h : 1;
}

void ui_set_text_mode(bool on) { g.text_mode = on; }
bool ui_text_mode(void) { return g.text_mode; }
void ui_set_enabled(bool on) { g.enabled = on; }
bool ui_enabled(void) { return g.enabled; }

static void set_btn(int b, bool down)
{
    if (b < 0 || b >= BTN_COUNT)
        return;
    if (down && !g.held[b]) {
        g.ev_pressed[b] = true;
        g.hold_since[b] = SDL_GetTicks64();
        g.last_repeat[b] = g.hold_since[b];
    }
    g.held[b] = down;
}

static int map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_z: return BTN_CONFIRM;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_x: return BTN_BACK;
    case SDLK_SPACE: return BTN_TRIANGLE;
    case SDLK_c: case SDLK_TAB: return BTN_SQUARE;
    case SDLK_q: return BTN_L;
    case SDLK_e: return BTN_R;
    case SDLK_n: case SDLK_F1: return BTN_START;
    case SDLK_m: case SDLK_F2: return BTN_SELECT;
    default: return -1;
    }
}

static int map_pad(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return BTN_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return BTN_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return BTN_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return BTN_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return BTN_CONFIRM;
    case SDL_CONTROLLER_BUTTON_B: return BTN_BACK;
    case SDL_CONTROLLER_BUTTON_Y: return BTN_TRIANGLE;
    case SDL_CONTROLLER_BUTTON_X: return BTN_SQUARE;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return BTN_L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return BTN_R;
    case SDL_CONTROLLER_BUTTON_START: return BTN_START;
    case SDL_CONTROLLER_BUTTON_BACK: return BTN_SELECT;
    default: return -1;
    }
}

static void pointer_down(float x, float y)
{
    g.down = true;
    g.ev_press = true;
    g.px = g.press_x = x;
    g.py = g.press_y = y;
    g.press_time = SDL_GetTicks64();
    g.drag_axis = 0;
    g.long_fired = false;
    g.vx = g.vy = 0;
    g.focus_visible = false;
}

static void pointer_move(float x, float y)
{
    if (g.down) {
        g.acc_dx += x - g.px;
        g.acc_dy += y - g.py;
        if (!g.drag_axis && !g.long_fired) {
            float ddx = x - g.press_x, ddy = y - g.press_y;
            if (ddx * ddx + ddy * ddy > DRAG_SLOP * DRAG_SLOP)
                g.drag_axis = fabsf(ddx) > fabsf(ddy) ? AXIS_X : AXIS_Y;
        }
    }
    g.px = x;
    g.py = y;
}

static void pointer_up(float x, float y)
{
    pointer_move(x, y);
    g.down = false;
    g.ev_release = true;
    g.rel_x = x;
    g.rel_y = y;
    g.rel_drag_axis = g.drag_axis;
    g.rel_long_fired = g.long_fired;
}

void ui_handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT && e->button.which != SDL_TOUCH_MOUSEID)
            pointer_down((float)e->button.x, (float)e->button.y);
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.which != SDL_TOUCH_MOUSEID)
            pointer_move((float)e->motion.x, (float)e->motion.y);
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT && e->button.which != SDL_TOUCH_MOUSEID && g.down)
            pointer_up((float)e->button.x, (float)e->button.y);
        break;
    case SDL_MOUSEWHEEL:
        g.acc_wheel_x += e->wheel.preciseX;
        g.acc_wheel_y += e->wheel.preciseY;
        break;
    case SDL_FINGERDOWN:
        /* Front panel only; the rear touchpad is a separate device. */
        if (e->tfinger.touchId == SDL_GetTouchDevice(0) && !g.finger_active) {
            g.finger_active = true;
            g.finger = e->tfinger.fingerId;
            pointer_down(e->tfinger.x * SCREEN_W, e->tfinger.y * SCREEN_H);
        }
        break;
    case SDL_FINGERMOTION:
        if (g.finger_active && e->tfinger.fingerId == g.finger)
            pointer_move(e->tfinger.x * SCREEN_W, e->tfinger.y * SCREEN_H);
        break;
    case SDL_FINGERUP:
        if (g.finger_active && e->tfinger.fingerId == g.finger) {
            g.finger_active = false;
            pointer_up(e->tfinger.x * SCREEN_W, e->tfinger.y * SCREEN_H);
        }
        break;
    case SDL_KEYDOWN:
        /* Vita: SDL's IME may emit key events; buttons come from the pad. */
        if (!g.text_mode && !e->key.repeat && !platform_is_vita())
            set_btn(map_key(e->key.keysym.sym), true);
        break;
    case SDL_KEYUP:
        if (!platform_is_vita())
            set_btn(map_key(e->key.keysym.sym), false);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        set_btn(map_pad(e->cbutton.button), true);
        break;
    case SDL_CONTROLLERBUTTONUP:
        set_btn(map_pad(e->cbutton.button), false);
        break;
    case SDL_CONTROLLERAXISMOTION: {
        float v = e->caxis.value / 32767.0f;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g.stick_x = v;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g.stick_y = v;
        break;
    }
    case SDL_JOYBUTTONDOWN:
        if (platform_is_vita())
            set_btn(platform_map_vita_button(e->jbutton.button), true);
        break;
    case SDL_JOYBUTTONUP:
        if (platform_is_vita())
            set_btn(platform_map_vita_button(e->jbutton.button), false);
        break;
    case SDL_JOYAXISMOTION:
        if (platform_is_vita()) {
            float v = e->jaxis.value / 32767.0f;
            if (e->jaxis.axis == 0) g.stick_x = v;
            if (e->jaxis.axis == 1) g.stick_y = v;
        }
        break;
    default:
        break;
    }
}

static float dead(float v) { return fabsf(v) < 0.25f ? 0 : (v - (v > 0 ? 0.25f : -0.25f)) / 0.75f; }

static const focusable *find_prev(uint32_t id)
{
    for (int i = 0; i < g.prev_n; i++)
        if (g.prev[i].id == id)
            return &g.prev[i];
    return NULL;
}

static void reveal(const focusable *f)
{
    for (int i = 0; i < f->nsc; i++) {
        const scroll_ctx *c = &f->sc[i];
        scroller *s = c->s;
        bool x = c->ax == AXIS_X;
        float view = x ? c->vp.w : c->vp.h;
        float start = (x ? f->r.x - c->vp.x : f->r.y - c->vp.y) + f->sc_pos[i];
        float len = x ? f->r.w : f->r.h;
        float margin = len < view * 0.5f ? 24.0f : 0.0f;
        float cur = s->has_target ? s->target : s->pos;
        float t = cur;
        if (start - margin < cur)
            t = start - margin;
        else if (start + len + margin > cur + view)
            t = start + len + margin - view;
        if (t < 0) t = 0;
        if (t > s->max) t = s->max;
        if (t != cur) {
            s->target = t;
            s->has_target = true;
            s->vel = 0;
        }
    }
}

static void navigate(int dir)
{
    const focusable *cur = find_prev(g.focus);
    if (!cur) {
        if (g.prev_n > 0) {
            g.focus = g.prev[0].id;
            reveal(&g.prev[0]);
        }
        return;
    }
    if ((cur->flags & UI_F_SLIDER) && (dir == BTN_LEFT || dir == BTN_RIGHT)) {
        g.slide = dir == BTN_LEFT ? -1 : 1;
        return;
    }
    rectf a = cur->r;
    float acx = a.x + a.w / 2, acy = a.y + a.h / 2;
    const focusable *best = NULL;
    float best_score = 1e30f;
    for (int i = 0; i < g.prev_n; i++) {
        const focusable *c = &g.prev[i];
        if (c->id == cur->id)
            continue;
        rectf b = c->r;
        float primary, orth_gap, orth_center;
        switch (dir) {
        case BTN_RIGHT:
            if (b.x < a.x + a.w - 4) continue;
            primary = b.x - (a.x + a.w);
            orth_gap = fmaxf(0, fmaxf(a.y, b.y) - fminf(a.y + a.h, b.y + b.h));
            orth_center = fabsf(b.y + b.h / 2 - acy);
            break;
        case BTN_LEFT:
            if (b.x + b.w > a.x + 4) continue;
            primary = a.x - (b.x + b.w);
            orth_gap = fmaxf(0, fmaxf(a.y, b.y) - fminf(a.y + a.h, b.y + b.h));
            orth_center = fabsf(b.y + b.h / 2 - acy);
            break;
        case BTN_DOWN:
            if (b.y < a.y + a.h - 4) continue;
            primary = b.y - (a.y + a.h);
            orth_gap = fmaxf(0, fmaxf(a.x, b.x) - fminf(a.x + a.w, b.x + b.w));
            orth_center = fabsf(b.x + b.w / 2 - acx);
            break;
        default: /* BTN_UP */
            if (b.y + b.h > a.y + 4) continue;
            primary = a.y - (b.y + b.h);
            orth_gap = fmaxf(0, fmaxf(a.x, b.x) - fminf(a.x + a.w, b.x + b.w));
            orth_center = fabsf(b.x + b.w / 2 - acx);
            break;
        }
        if (primary < 0) primary = 0;
        float score = primary + orth_gap * 4.0f + orth_center * 0.15f;
        if (score < best_score) {
            best_score = score;
            best = c;
        }
    }
    if (best) {
        g.focus = best->id;
        reveal(best);
    }
}

void ui_begin_frame(float dt)
{
    g.dt = dt;
    g.now = SDL_GetTicks64();

    g.pressed = g.ev_press;
    g.released = g.ev_release;
    g.ev_press = g.ev_release = false;
    g.dx = g.acc_dx;
    g.dy = g.acc_dy;
    g.acc_dx = g.acc_dy = 0;
    g.wheel_x = g.acc_wheel_x;
    g.wheel_y = g.acc_wheel_y;
    g.acc_wheel_x = g.acc_wheel_y = 0;
    if (dt > 0 && g.down) {
        g.vx = g.vx * 0.6f + (g.dx / dt) * 0.4f;
        g.vy = g.vy * 0.6f + (g.dy / dt) * 0.4f;
    }

    g.long_this_frame = false;
    if (g.down && !g.drag_axis && !g.long_fired && g.now - g.press_time > LONG_PRESS_MS) {
        g.long_fired = true;
        g.long_this_frame = true;
    }

    g.click_pending = g.click_cur;
    g.menu_pending = g.menu_cur;
    g.click_cur = g.menu_cur = 0;
    g.hot_prev = g.hot_cur;
    g.hot_cur = 0;

    for (int b = 0; b < BTN_COUNT; b++) {
        g.pressed_b[b] = g.ev_pressed[b];
        g.ev_pressed[b] = false;
        bool dir = b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT;
        if (dir && g.held[b] && !g.pressed_b[b] && g.now - g.hold_since[b] > REPEAT_DELAY_MS &&
            g.now - g.last_repeat[b] > REPEAT_RATE_MS) {
            g.pressed_b[b] = true;
            g.last_repeat[b] = g.now;
        }
    }

    apply_combo();

    g.slide = 0;
    if (g.text_mode)
        return;
    static const btn dirs[4] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
    for (int i = 0; i < 4; i++) {
        if (!g.pressed_b[dirs[i]])
            continue;
        if (!g.focus_visible) {
            /* First press only reveals focus where it was. */
            g.focus_visible = true;
            if (!find_prev(g.focus) && g.prev_n > 0) {
                g.focus = g.prev[0].id;
                reveal(&g.prev[0]);
            }
        } else {
            navigate(dirs[i]);
        }
        g.pressed_b[dirs[i]] = false;
        break;
    }
    if (g.pressed_b[BTN_CONFIRM] && !g.focus_visible && g.prev_n > 0) {
        g.focus_visible = true;
        if (!find_prev(g.focus))
            g.focus = g.prev[0].id;
        g.pressed_b[BTN_CONFIRM] = false;
    }
}

void ui_end_frame(void)
{
    memcpy(g.prev, g.cur, sizeof(focusable) * (size_t)g.cur_n);
    g.prev_n = g.cur_n;
    g.cur_n = 0;
    g.clip_n = 0;
    g.sn = 0;
}

bool ui_btn(btn b) { return g.pressed_b[b]; }
void ui_consume(btn b) { g.pressed_b[b] = false; }

void ui_push_clip(rectf r)
{
    if (g.clip_n > 0)
        r = rect_intersect(r, g.clip[g.clip_n - 1]);
    if (g.clip_n < CLIP_STACK)
        g.clip[g.clip_n++] = r;
}

void ui_pop_clip(void)
{
    if (g.clip_n > 0)
        g.clip_n--;
}

static rectf cur_clip(void)
{
    return g.clip_n ? g.clip[g.clip_n - 1] : rect(0, 0, SCREEN_W, SCREEN_H);
}

ui_res ui_item(uint32_t id, rectf r, unsigned flags)
{
    ui_res res = { 0 };
    if (!g.enabled)
        return res;

    if (!(flags & UI_F_NOFOCUS) && g.cur_n < MAX_FOC) {
        focusable *f = &g.cur[g.cur_n++];
        f->id = id;
        f->r = r;
        f->flags = flags;
        f->nsc = g.sn;
        for (int i = 0; i < g.sn; i++) {
            f->sc[i] = g.sstack[i];
            f->sc_pos[i] = g.sstack[i].s->pos;
        }
    }

    rectf hit = rect_intersect(r, cur_clip());
    bool press_in = rect_contains(hit, g.press_x, g.press_y);
    if (g.down && press_in && !g.drag_axis && !g.long_fired)
        g.hot_cur = id;
    if (g.released && press_in && !g.rel_drag_axis && !g.rel_long_fired &&
        rect_contains(hit, g.rel_x, g.rel_y))
        g.click_cur = id;
    if (g.long_this_frame && press_in)
        g.menu_cur = id;

    res.pressed = g.hot_prev == id && g.down;
    res.clicked = g.click_pending == id;
    res.menu = g.menu_pending == id;

    if (g.focus == id && g.focus_visible) {
        res.focused = true;
        if (g.pressed_b[BTN_CONFIRM]) {
            res.clicked = true;
            g.pressed_b[BTN_CONFIRM] = false;
        }
        if (g.pressed_b[BTN_SQUARE]) {
            res.menu = true;
            g.pressed_b[BTN_SQUARE] = false;
        }
        if (flags & UI_F_SLIDER)
            res.slide = g.slide;
    }
    if (res.clicked || res.menu) {
        if (!(flags & UI_F_NOFOCUS))
            g.focus = id;
    }
    return res;
}

uint32_t ui_focus(void) { return g.focus; }
void ui_set_focus(uint32_t id) { g.focus = id; }
bool ui_focus_visible(void) { return g.focus_visible; }

void ui_reset_focus_list(void)
{
    /* Drop both lists: items registered so far this frame belong to the
     * screen being left, and ui_end_frame would otherwise keep them. */
    g.prev_n = 0;
    g.cur_n = 0;
    g.click_pending = g.click_cur = 0;
    g.menu_pending = g.menu_cur = 0;
}

bool ui_has_focusables(void) { return g.prev_n > 0; }

void ui_scroll_begin(scroller *s, rectf vp, float content_len, axis ax, bool stick)
{
    bool x = ax == AXIS_X;
    float view = x ? vp.w : vp.h;
    s->max = content_len - view > 0 ? content_len - view : 0;

    bool owns_drag = g.enabled && g.drag_axis == (int)ax &&
                     rect_contains(rect_intersect(vp, cur_clip()), g.press_x, g.press_y);
    bool owns_release = g.enabled && g.released && g.rel_drag_axis == (int)ax &&
                        rect_contains(rect_intersect(vp, cur_clip()), g.press_x, g.press_y);

    if (owns_drag && g.down) {
        s->pos -= x ? g.dx : g.dy;
        s->vel = 0;
        s->has_target = false;
        /* Rubber band: resist beyond the ends. */
        if (s->pos < 0) s->pos *= 0.6f;
        if (s->pos > s->max) s->pos = s->max + (s->pos - s->max) * 0.6f;
    } else {
        if (owns_release)
            s->vel = -(x ? g.vx : g.vy);
        if (s->vel != 0) {
            s->pos += s->vel * g.dt;
            s->vel *= powf(0.04f, g.dt);
            if (fabsf(s->vel) < 8)
                s->vel = 0;
        }
        if (g.enabled && rect_contains(vp, g.px, g.py)) {
            float w = x ? (g.wheel_x != 0 ? -g.wheel_x : g.wheel_y) : g.wheel_y;
            if (w != 0 && (x ? g.wheel_x != 0 : 1)) {
                s->pos -= w * 70;
                s->has_target = false;
                s->vel = 0;
            }
        }
        if (stick && g.enabled && !x && dead(g.stick_y) != 0) {
            s->pos += dead(g.stick_y) * 900 * g.dt;
            s->has_target = false;
        }
        if (s->has_target) {
            float k = g.dt * 14 > 1 ? 1 : g.dt * 14;
            s->pos += (s->target - s->pos) * k;
            if (fabsf(s->target - s->pos) < 0.5f) {
                s->pos = s->target;
                s->has_target = false;
            }
        }
        /* Settle back from rubber band. */
        if (s->pos < 0) {
            s->pos += (0 - s->pos) * (g.dt * 12 > 1 ? 1 : g.dt * 12);
            if (s->pos > -0.5f) s->pos = 0;
            s->vel = 0;
        } else if (s->pos > s->max) {
            s->pos += (s->max - s->pos) * (g.dt * 12 > 1 ? 1 : g.dt * 12);
            if (s->pos < s->max + 0.5f) s->pos = s->max;
            s->vel = 0;
        }
    }

    if (g.sn < SCROLL_STACK) {
        g.sstack[g.sn].s = s;
        g.sstack[g.sn].vp = vp;
        g.sstack[g.sn].ax = ax;
        g.sn++;
    }
}

void ui_scroll_end(void)
{
    if (g.sn > 0)
        g.sn--;
}

void ui_scroll_to(scroller *s, float pos)
{
    s->target = pos;
    s->has_target = true;
    s->vel = 0;
}

bool ui_pointer_down(void) { return g.enabled && g.down; }
float ui_pointer_x(void) { return g.px; }
float ui_pointer_y(void) { return g.py; }
bool ui_pointer_dragging(void) { return g.drag_axis != 0; }
bool ui_pointer_pressed_in(rectf r)
{
    return g.enabled && g.down && rect_contains(rect_intersect(r, cur_clip()), g.press_x, g.press_y);
}
