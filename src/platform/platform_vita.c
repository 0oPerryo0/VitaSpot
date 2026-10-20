#ifdef __vita__

#include "platform.h"
#include "vam_backend.h"

#include <psp2/apputil.h>
#include <psp2/avconfig.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/registrymgr.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <string.h>

#define DATA_DIR "ux0:data/VitaSpot/"
/* Data folder used before the app was renamed from VitaAM. */
#define OLD_DATA_DIR "ux0:data/VitaAM"

/* Lower runs first (user range 64..191). Backend pthreads default to 159,
 * which used to outrank this thread's 160 and stall frames; the backend now
 * drops its network threads to 170 and lifts audio output to 80. */
#define PRIORITY_UI 150
#define PRIORITY_BACKGROUND 170

static bool g_swap_confirm;

void platform_init(void)
{
    /* Max clocks: UI is light but the backend decodes audio and runs TLS. */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, PRIORITY_UI);

    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);

    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    g_swap_confirm = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;

    /* One-time move from the old folder so the saved login and caches
       survive the rename. Only when the new folder doesn't exist yet. */
    SceIoStat st;
    if (sceIoGetstat("ux0:data/VitaSpot", &st) < 0 && sceIoGetstat(OLD_DATA_DIR, &st) >= 0)
        sceIoRename(OLD_DATA_DIR, "ux0:data/VitaSpot");
    sceIoMkdir(DATA_DIR, 0777);
}

void platform_shutdown(void)
{
    sceAppUtilShutdown();
}

void platform_frame(bool keep_awake)
{
    /* The app dims and turns off the screen itself (power.c). */
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
    if (keep_awake)
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

bool platform_is_vita(void) { return true; }

static screen_level g_screen = SCREEN_ON;

/* The brightness the user set in Settings. Setting the backlight doesn't
   change it, so the full level can always be restored from here. */
static int user_brightness(void)
{
    int v = 0;
    if (sceRegMgrGetKeyInt("/CONFIG/DISPLAY", "brightness", &v) < 0 || v < 21)
        return 0;
    return v;
}

/* Only the backlight changes. scePowerRequestDisplayOff() also silenced the
   app's audio, which is the whole point of turning the screen off. */
void platform_set_screen(screen_level level)
{
    if (level == g_screen)
        return;
    int full = user_brightness();
    int on = 0, rc;
    if (level == SCREEN_OFF) {
        rc = sceAVConfigSetDisplayBrightness(0); /* 0 turns the screen off */
    } else {
        /* Brightness 0 powers the panel down; a new level alone doesn't
           bring it back. */
        if (g_screen == SCREEN_OFF)
            on = scePowerRequestDisplayOn();
        int target = !full ? 65536 : level == SCREEN_DIM ? (full / 4 < 21 ? 21 : full / 4) : full;
        rc = sceAVConfigSetDisplayBrightness(target);
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "display level %d (from %d): brightness rc %#x, display on rc %#x, user %d", level,
             g_screen, rc, on, full);
    vam_log(msg);
    g_screen = level;
}

const char *platform_btn_label(btn b)
{
    switch (b) {
    case BTN_UP: return "Up";
    case BTN_DOWN: return "Down";
    case BTN_LEFT: return "Left";
    case BTN_RIGHT: return "Right";
    case BTN_CONFIRM: return g_swap_confirm ? "Circle" : "Cross";
    case BTN_BACK: return g_swap_confirm ? "Cross" : "Circle";
    case BTN_TRIANGLE: return "Triangle";
    case BTN_SQUARE: return "Square";
    case BTN_L: return "L";
    case BTN_R: return "R";
    case BTN_START: return "Start";
    case BTN_SELECT: return "Select";
    default: return "?";
    }
}

void platform_lower_thread_priority(void)
{
    sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, PRIORITY_BACKGROUND);
}

const char *platform_asset_path(const char *rel, char *buf, size_t size)
{
    snprintf(buf, size, "app0:assets/%s", rel);
    return buf;
}

const char *platform_data_dir(void) { return DATA_DIR; }

/* SDL2's Vita joystick button order. */
int platform_map_vita_button(int b)
{
    switch (b) {
    case 0: return BTN_TRIANGLE;
    case 1: return g_swap_confirm ? BTN_CONFIRM : BTN_BACK;  /* Circle */
    case 2: return g_swap_confirm ? BTN_BACK : BTN_CONFIRM;  /* Cross  */
    case 3: return BTN_SQUARE;
    case 4: return BTN_L;
    case 5: return BTN_R;
    case 6: return BTN_DOWN;
    case 7: return BTN_LEFT;
    case 8: return BTN_UP;
    case 9: return BTN_RIGHT;
    case 10: return BTN_SELECT;
    case 11: return BTN_START;
    default: return -1;
    }
}

#endif
