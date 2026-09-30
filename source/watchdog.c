/* watchdog.c -- make a hang report itself.
 *
 * A crash leaves a report; a hang leaves nothing, and a hang that shows a
 * black screen is the hardest thing to diagnose from someone else's console.
 * The idea and most of the shape here come from the Happy Wheels Switch
 * port's watchdog.c, which in turn credits the Fruit Ninja port's diag.c.
 *
 * WHAT IT WATCHES
 *
 * That port's render loop was its own, so it published a frame counter
 * directly. This port never sees a frame: the game renders. But every frame
 * the game presents has to go through eglSwapBuffers, and SDL reaches EGL by
 * dlopen()ing libEGL.so and dlsym()ing each entry point out of imports.c --
 * so the swap is interposable, and counting it is counting frames.
 *
 * Together with log_stage()'s breadcrumbs that distinguishes the three cases
 * a black screen can be:
 *
 *   no swaps, stage is early      start-up wedged before the game rendered
 *   no swaps, stage is hxcpp_main the game is running but never presents
 *   swaps climbing, still black   it IS presenting; the problem is content,
 *                                 or the crop/transform, not the pipeline
 *
 * The thread is a raw libnx Thread rather than a pthread, deliberately: if
 * the hang is a deadlock inside the pthread shim, a pthread watchdog would
 * be stuck in it too.
 *
 * Cost on the happy path is one relaxed store per frame.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "nativewindow.h"
#include "watchdog.h"

#define STALL_SECONDS 5

static atomic_uint  g_swaps;
static atomic_bool  g_running;
static atomic_bool  g_started;
static Thread       g_thread;

void watchdog_note_swap(void)
{
    atomic_fetch_add_explicit(&g_swaps, 1u, memory_order_relaxed);
}

unsigned watchdog_swaps(void)
{
    return atomic_load_explicit(&g_swaps, memory_order_relaxed);
}

static void watchdog_main(void *arg)
{
    unsigned last = 0, quiet = 0;
    int reported = 0;
    char last_stage[80] = "";

    (void)arg;
    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        unsigned now;
        const char *stage;

        svcSleepThread(1000000000ULL);
        now = atomic_load_explicit(&g_swaps, memory_order_relaxed);
        stage = log_last_stage();

        if (now != last || strncmp(stage, last_stage, sizeof(last_stage) - 1) != 0) {
            last = now;
            snprintf(last_stage, sizeof(last_stage), "%s", stage);
            quiet = 0;
            reported = 0;
            continue;
        }

        if (++quiet < STALL_SECONDS || reported)
            continue;

        LOGE("*** STALL: nothing has moved for %u s. "
             "frames presented=%u, last stage=\"%s\" ***", quiet, now, stage);
        if (now == 0)
            LOGE("*** the game has never presented a frame. If the last stage "
                 "is hxcpp_main it reached the game but never got a GL "
                 "surface; check for an eglCreateWindowSurface failure above. ***");
        pbnw_report("at the stall");
        log_flush();
        reported = 1;          /* one report per stall, not one a second */
    }
}

void watchdog_start(void)
{
    if (atomic_exchange(&g_started, true))
        return;
    atomic_store(&g_running, true);
    if (R_FAILED(threadCreate(&g_thread, watchdog_main, NULL, NULL, 0x4000, 0x2C, -2))) {
        LOGI("watchdog: could not start (not fatal)");
        atomic_store(&g_running, false);
        return;
    }
    if (R_FAILED(threadStart(&g_thread))) {
        threadClose(&g_thread);
        atomic_store(&g_running, false);
        return;
    }
    LOGI("watchdog: running; a stall of %d s will be reported", STALL_SECONDS);
}

void watchdog_stop(void)
{
    atomic_store(&g_running, false);
}
