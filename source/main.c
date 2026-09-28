/* main.c -- start-up and shutdown.
 *
 *   1. config.txt, leveldevil.log, check the game files, chdir into assets/
 *   2. bionic environment (SDL_DYNAMIC_API, ALSOFT_DRIVERS), import table,
 *      JNI runtime
 *   3. load liblime.so and libApplicationMain.so, relocate, resolve every
 *      import, map them executable
 *   4. hand liblime to the SDL and JNI bridges, run both modules' static
 *      constructors (on the main thread, as Android's System.loadLibrary does)
 *   5. run hxcpp_main -- the whole game -- on its own thread with a large
 *      stack, the way SDLActivity runs it off the UI thread
 *   6. the main thread sleeps until something asks to exit, then ends the
 *      process
 *
 * Why the process ends with svcExitProcess instead of returning from main:
 * the game leaves threads running (hxcpp's GC workers, the SDL audio thread,
 * the JNI UI thread). Returning to the homebrew loader unmaps the code those
 * threads are executing, and the next thing any of them does is crash inside
 * hbmenu. Terminating the process is the only exit that cannot race them.
 *
 * MIT licensed, see LICENSE.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "app.h"
#include "bionic.h"
#include "config.h"
#include "imports.h"
#include "input.h"
#include "jni_env.h"
#include "log.h"
#include "nx_pointer.h"
#include "paths.h"
#include "sdl_bridge.h"
#include "so_util.h"

#define QUIT_WATCHDOG_NS (8ULL * 1000000000ULL)

static so_module g_lime, g_app;
static Handle    g_main_thread;
static u64       g_start_tick;
static volatile int g_exit_requested;
static volatile int g_exit_code;
static volatile u64 g_quit_tick;

/* ------------------------------------------------------------ app.h ---- */

void hs_request_exit(int code)
{
    if (!g_exit_requested) {
        g_exit_code = code;
        g_exit_requested = 1;
    }
}

int hs_exit_requested(void) { return g_exit_requested; }
int hs_exit_code(void)      { return g_exit_code; }

void hs_note_quit(void)
{
    if (!g_quit_tick)
        g_quit_tick = armGetSystemTick();
}

double hs_uptime(void)
{
    return (double)armTicksToNs(armGetSystemTick() - g_start_tick) / 1e9;
}

static void terminate(int code) __attribute__((noreturn));
static void terminate(int code)
{
    LOGI("exit (code %d) after %.1f s", code, hs_uptime());
    log_flush();
    svcExitProcess();
}

void hs_park_forever(void)
{
    if (threadGetCurHandle() == g_main_thread)
        terminate(g_exit_code);
    for (;;)
        svcSleepThread(1000000000LL);
}

/* ------------------------------------------------------- error screen -- */

static int error_screen(const char *msg)
{
    PadState pad;
    LOGE("%s", msg);
    log_flush();

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\x1b[2J\x1b[1;1H");
    printf("Level Devil for Nintendo Switch could not start.\n\n");
    printf("%s\n\n", msg);
    if (paths_log()[0])
        printf("Details are in %s\n\n", paths_log());
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

/* ------------------------------------------------------------- loader -- */

static int load_module(so_module *m, const char *path, const char *name, char *err, size_t errlen)
{
    int rc = so_load(m, path, name);
    if (rc == -3) {
        snprintf(err, errlen, "Not enough memory to load %s.\n\n"
                 "Launch homebrew by holding R while opening a game (title\n"
                 "takeover); the album applet has far less memory.", name);
        return -1;
    }
    if (rc != 0) {
        snprintf(err, errlen, "%s could not be loaded (error %d).\n"
                 "Is it the arm64-v8a build from the APK's lib/arm64-v8a folder?", path, rc);
        return -1;
    }
    if (so_relocate(m) != 0) {
        snprintf(err, errlen, "%s contains relocations this loader does not handle.", name);
        return -1;
    }
    return 0;
}

static int resolve_module(so_module *m, char *err, size_t errlen)
{
    char missing[1024];
    int n = so_resolve(m, imports_resolve, missing, sizeof(missing));
    if (n) {
        snprintf(err, errlen, "%s needs %d function(s) this port does not provide:\n%s\n\n"
                 "This usually means a different game version. Run\n"
                 "nm -D --undefined-only on its libraries.", m->name, n, missing);
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
    if (at != AppletType_Application && at != AppletType_SystemApplication)
        LOGI("system: running as an applet; if the game runs out of memory, launch "
             "hbmenu by holding R while starting a game");
}

/* -------------------------------------------------------- game thread -- */

typedef void (*EntryFn)(void);

/* Keep the game thread off the core that runs the audio thread and the
 * system's applet callbacks. Those preempt it -- OpenAL mixes a buffer every
 * ~23 ms at a higher priority -- and a frame that was close to the refresh
 * deadline misses it, which reads as judder at a steady 60. */
static void pin_game_thread(void)
{
    u64 mask = 0;
    int core = g_cfg.game_core;

    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !mask) {
        LOGI("thread: could not read the core mask; leaving the game thread where it is");
        return;
    }
    if (core < 0) {
        /* Highest core this process may use, other than 0. */
        for (core = 3; core > 0; core--)
            if (mask & (1ULL << core))
                break;
    }
    if (core <= 0 || !(mask & (1ULL << core))) {
        LOGI("thread: core %d not available (mask 0x%lx); staying on the default core",
             core, (unsigned long)mask);
        return;
    }
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1U << core)))
        LOGI("thread: could not move the game thread to core %d", core);
    else
        LOGI("thread: game thread pinned to core %d (process cores 0x%lx)", core,
             (unsigned long)mask);
}

static void *game_thread(void *arg)
{
    EntryFn entry = *(EntryFn *)arg;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    pin_game_thread();
    LOGI("game thread: calling hxcpp_main");
    log_flush();
    entry();
    LOGI("game thread: hxcpp_main returned");
    hs_request_exit(0);
    return NULL;
}

/* ---------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    static EntryFn entry;
    char err[2048];
    pthread_attr_t attr;
    pthread_t thread;
    int rc, flush_ticks = 0;

    g_start_tick = armGetSystemTick();
    g_main_thread = threadGetCurHandle();

    config_defaults();
    /* Before anything can be logged: the log lives in the game folder. */
    if (!paths_locate(argc, argv, err, sizeof(err)))
        return error_screen(err);

    log_init(paths_log());
    config_load(paths_config());
    log_set_level(g_cfg.log_level);
    LOGI("port build %s %s", __DATE__, __TIME__);
    LOGI("launched as %s", (argc > 0 && argv && argv[0]) ? argv[0] : "(no argv)");
    log_system_info();

    if (!paths_init(err, sizeof(err)))
        return error_screen(err);

    bx_libc_init();
    imports_init();
    jni_init();

    if (load_module(&g_lime, paths_lib_lime(), "liblime.so", err, sizeof(err)) != 0 ||
        load_module(&g_app, paths_lib_app(), "libApplicationMain.so", err, sizeof(err)) != 0 ||
        resolve_module(&g_lime, err, sizeof(err)) != 0 ||
        resolve_module(&g_app, err, sizeof(err)) != 0)
        return error_screen(err);

    if (so_finalize(&g_lime) != 0 || so_finalize(&g_app) != 0)
        return error_screen("Mapping the game code failed (svcMapProcessCodeMemory).\n"
                            "Atmosphere is required.");

    entry = (EntryFn)so_symbol(&g_app, "hxcpp_main");
    if (!entry)
        return error_screen("libApplicationMain.so has no hxcpp_main export.");

    nxp_init(1280, 720, paths_root());   /* reads cursor.png if it is there */
    sdlb_init(&g_lime);
    jni_bind_lime(&g_lime);
    input_init();
    SDL_SetMainReady();

    so_run_init_array(&g_lime);
    so_run_init_array(&g_app);
    LOGI("modules ready, starting the game");

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)g_cfg.game_stack_mb * 1024 * 1024);
    rc = pthread_create(&thread, &attr, game_thread, &entry);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE("could not create the game thread (%d) with a %d MB stack", rc, g_cfg.game_stack_mb);
        terminate(1);
    }

    while (!g_exit_requested) {
        svcSleepThread(50000000LL);
        /* Flushing the log writes to the SD card, which can block for
         * milliseconds. Doing it here keeps it off the thread that renders. */
        if (++flush_ticks >= 20) {
            flush_ticks = 0;
            log_flush();
        }
        if (g_quit_tick && armTicksToNs(armGetSystemTick() - g_quit_tick) > QUIT_WATCHDOG_NS) {
            LOGI("the game did not exit %llu s after SDL_QUIT; exiting anyway",
                 (unsigned long long)(QUIT_WATCHDOG_NS / 1000000000ULL));
            hs_request_exit(0);
        }
    }

    /* Give a clean shutdown in progress (SDL_Quit closing audio) a moment. */
    svcSleepThread(250000000LL);
    terminate(g_exit_code);
}
