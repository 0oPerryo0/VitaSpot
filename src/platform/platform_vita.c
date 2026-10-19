#ifdef __vita__

#include "platform.h"

#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
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
    /* Screen may still dim/turn off, but the system will not suspend us. */
    if (keep_awake)
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

bool platform_is_vita(void) { return true; }

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
