#include <SDL.h>
#include <stdio.h>

#include "app.h"
#include "gfx/gfx.h"
#include "gfx/images.h"
#include "gfx/text.h"
#include "platform/devscript.h"
#include "platform/platform.h"
#include "theme.h"
#include "ui/ui.h"

/* Frames slower than this get a line in vam.log with where the time went. */
#define SLOW_FRAME_MS 45.0
#define SLOW_LOG_INTERVAL_MS 250

typedef struct slow_frames {
    Uint64 last_log;
    int suppressed;
} slow_frames;

static double ms_between(Uint64 a, Uint64 b, double freq) { return (double)(b - a) * 1000.0 / freq; }

static void note_frame(slow_frames *sf, double events_ms, double draw_ms, double present_ms)
{
    double total = events_ms + draw_ms + present_ms;
    if (total < SLOW_FRAME_MS)
        return;
    Uint64 now = SDL_GetTicks64();
    if (now - sf->last_log < SLOW_LOG_INTERVAL_MS) {
        sf->suppressed++;
        return;
    }
    char msg[200];
    snprintf(msg, sizeof(msg),
             "slow frame %.0f ms (events %.0f, update+draw %.0f, present %.0f; text renders %d, image uploads %d)%s",
             total, events_ms, draw_ms, present_ms, text_renders_this_frame(), images_uploads_this_frame(),
             sf->suppressed ? " +more" : "");
    vam_log(msg);
    sf->last_log = now;
    sf->suppressed = 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    platform_init();

    /* Touch arrives as finger events; don't also synthesise mouse clicks. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }

    Uint32 wflags = platform_is_vita() ? 0 : SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window *win = SDL_CreateWindow("VitaSpot", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W,
                                       SCREEN_H, wflags);
    if (!win) {
        SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        SDL_Log("SDL_CreateRenderer: %s", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);

    if (platform_is_vita()) {
        if (SDL_NumJoysticks() > 0)
            SDL_JoystickOpen(0);
    } else {
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i))
                SDL_GameControllerOpen(i);
    }

    gfx_init(ren);
    text_init();
    images_init();
    ui_init();
    app_init(win, ren);
    devscript_init();

    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    slow_frames sf = { 0 };
    while (!A.quit) {
        Uint64 t0 = SDL_GetPerformanceCounter();
        devscript_frame(&A.quit);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED && !platform_is_vita())
                SDL_GameControllerOpen(e.cdevice.which);
            app_handle_sdl_event(&e);
        }
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - last) / freq);
        last = now;
        if (dt > 0.1f)
            dt = 0.1f;

        ui_begin_frame(dt);
        app_frame(dt);
        ui_end_frame();
        devscript_after_render(ren);
        Uint64 t_drawn = SDL_GetPerformanceCounter();
        if (app_drawing()) {
            SDL_RenderPresent(ren);
            gfx_flush_destroyed(false);
        } else {
            SDL_Delay(50); /* no vsync to pace the loop while the screen is off */
        }
        Uint64 t_end = SDL_GetPerformanceCounter();
        note_frame(&sf, ms_between(t0, now, freq), ms_between(now, t_drawn, freq),
                   ms_between(t_drawn, t_end, freq));
    }

    app_shutdown();
    images_shutdown();
    text_shutdown();
    gfx_flush_destroyed(true);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    platform_shutdown();
    return 0;
}
