/* main.c -- start-up and shutdown.
 *
 *   1. config.txt, leveldevil.log, find the game files, chdir into assets/
 *   2. take the Switch's window; build the import table and the fake Java
 *      runtime; give the main thread a bionic thread pointer
 *   3. load libLevelDevil.so, relocate, resolve every import, map it
 *      executable, run its constructors
 *   4. be the Android framework: ANativeActivity_onCreate and the lifecycle
 *      (native_activity.c). The glue starts android_main -- the whole Defold
 *      engine -- on its own thread
 *   5. the main thread becomes the "UI thread": the applet loop, input, dock
 *      changes, log flushing, until something asks to exit
 *
 * The process ends with svcExitProcess rather than returning from main: the
 * engine leaves threads running, and returning to the homebrew loader would
 * unmap the code they are executing.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "bionic.h"
#include "config.h"
#include "imports.h"
#include "input.h"
#include "jni_env.h"
#include "log.h"
#include "native_activity.h"
#include "nativewindow.h"
#include "nx_pointer.h"
#include "paths.h"
#include "so_util.h"
#include "tls.h"
#include "watchdog.h"

static so_module g_engine;
static Handle    g_main_thread;
static u64       g_start_tick;
static volatile int g_exit_requested;
static volatile int g_exit_code;

/* ------------------------------------------------------------- app.h ---- */

void pb_request_exit(int code)
{
    if (!g_exit_requested) {
        g_exit_code = code;
        g_exit_requested = 1;
    }
}

int  pb_exit_requested(void) { return g_exit_requested; }
int  pb_exit_code(void)      { return g_exit_code; }
void pb_note_quit(void)      { }
void na_on_finish(void)      { pb_request_exit(0); }

double pb_uptime(void)
{
    return (double)armTicksToNs(armGetSystemTick() - g_start_tick) / 1e9;
}

static void terminate(int code) __attribute__((noreturn));
static void terminate(int code)
{
    LOGI("exit (code %d) after %.1f s", code, pb_uptime());
    watchdog_stop();
    log_flush();
    svcExitProcess();
    __builtin_unreachable();
}

void pb_park_forever(void)
{
    if (threadGetCurHandle() == g_main_thread)
        terminate(g_exit_code);
    for (;;)
        svcSleepThread(1000000000LL);
}

/* -------------------------------------------------------- error screen -- */

static int error_screen(const char *msg)
{
    PadState pad;
    LOGE("%s", msg);
    log_flush();

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\x1b[2J\x1b[1;1H");
    printf("Level Devil for Nintendo Switch could not start.\n\n%s\n\n", msg);
    if (paths_log()[0])
        printf("Details are in %s (set log_level = 2 in config.txt)\n\n", paths_log());
    printf("Press + to exit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
        svcSleepThread(16000000LL);
    }
    consoleExit(NULL);
    return 1;
}

/* -------------------------------------------------------------- loader -- */

static int load_engine(char *err, size_t errlen)
{
    char missing[1024];
    int rc = so_load(&g_engine, paths_lib(), "libLevelDevil.so"), n;

    if (rc == -3) {
        snprintf(err, errlen, "Not enough memory to load libLevelDevil.so.\n\n"
                 "Launch homebrew by holding R while opening a game (title\n"
                 "takeover); the album applet has far less memory.");
        return -1;
    }
    if (rc != 0) {
        snprintf(err, errlen, "%s could not be loaded (error %d).\n"
                 "Is it the arm64-v8a build from split_config.arm64_v8a.apk?", paths_lib(), rc);
        return -1;
    }
    if (so_relocate(&g_engine) != 0) {
        snprintf(err, errlen, "libLevelDevil.so contains relocations this loader does not handle.");
        return -1;
    }
    n = so_resolve(&g_engine, imports_resolve, missing, sizeof(missing));
    if (n) {
        snprintf(err, errlen, "libLevelDevil.so needs %d function(s) this port does not provide:\n%s\n\n"
                 "This usually means a different game version.", n, missing);
        return -1;
    }
    if (so_finalize(&g_engine) != 0) {
        snprintf(err, errlen, "Mapping the game code failed (svcMapProcessCodeMemory).\n"
                 "Atmosphere is required.");
        return -1;
    }
    return 0;
}

static void log_system_info(void)
{
    u64 total = 0, used = 0;
    u32 hv = hosversionGet();
    AppletType at = appletGetAppletType();
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    LOGI("system: HOS %u.%u.%u, %s, %s mode, memory %lu MB total / %lu MB used",
         HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
         (at == AppletType_Application || at == AppletType_SystemApplication)
             ? "application (full memory)" : "applet mode (limited memory)",
         appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
         (unsigned long)(total >> 20), (unsigned long)(used >> 20));
}

/* ---------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    char err[2048];
    int flush_ticks = 0, focused = 1, stats_ticks = 0;
    unsigned stats_swaps = 0;
    u64 stats_tick = armGetSystemTick();

    g_start_tick = armGetSystemTick();
    g_main_thread = threadGetCurHandle();

    config_defaults();
    if (!paths_locate(argc, argv, err, sizeof(err)))
        return error_screen(err);

    log_init(paths_log());
    config_load(paths_config());
    log_set_level(g_cfg.log_level);
    LOGI("port build %s %s", __DATE__, __TIME__);
    log_system_info();

    log_stage("paths_init");
    if (!paths_init(err, sizeof(err)))
        return error_screen(err);

    log_stage("pbnw_init (taking the Switch window)");
    if (!pbnw_init())
        return error_screen("The Switch's window could not be set up.");

    log_stage("imports and JNI");
    bx_libc_init();
    bx_defold_init();
    imports_init();
    jni_init();

    log_stage("installing the bionic thread pointer");
    if (!pb_tls_attach("the main thread"))
        return error_screen("Could not allocate the thread-local block the "
                            "game's stack checks read.");
    pb_tls_selftest();

    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    log_stage("loading libLevelDevil.so");
    if (load_engine(err, sizeof(err)) != 0)
        return error_screen(err);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);

    input_init();
    nxp_init(pbnw_width(), pbnw_height(), paths_root());

    log_stage("constructors (init_array)");
    so_run_init_array(&g_engine);
    jni_bind_engine(&g_engine);
    LOGI("engine ready");

    watchdog_start();
    log_stage("ANativeActivity_onCreate and the lifecycle");
    if (!na_start(&g_engine))
        return error_screen("libLevelDevil.so has no ANativeActivity_onCreate.\n"
                            "Is this the Defold build of Level Devil?");
    log_stage("running");

    while (!g_exit_requested) {
        if (!appletMainLoop()) {
            LOGI("applet: close requested");
            break;
        }
        input_pump();
        jni_pump();

        {
            int now = appletGetFocusState() == AppletFocusState_InFocus;
            if (now != focused) {
                focused = now;
                na_focus(focused);
            }
        }
        if (pbnw_follow_operation_mode())
            na_window_resized();

        if (g_cfg.frame_stats && ++stats_ticks >= 625) {     /* ~5 s */
            unsigned swaps = watchdog_swaps(), slow, worst;
            pbnw_frame_stats(&slow, &worst);
            LOGI("frames: %.1f fps, %u slower than 20 ms, worst %u ms",
                 (swaps - stats_swaps) / (armTicksToNs(armGetSystemTick() - stats_tick) / 1e9),
                 slow, worst);
            stats_ticks = 0;
            stats_swaps = swaps;
            stats_tick = armGetSystemTick();
        }
        /* Flushing writes to the SD card; keep it off the render thread. */
        if (++flush_ticks >= 120) {
            flush_ticks = 0;
            log_flush();
        }
        svcSleepThread(8000000LL);
    }
    terminate(g_exit_code);
}
