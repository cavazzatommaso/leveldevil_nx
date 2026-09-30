/* nativewindow.c -- ANativeWindow, backed by the Switch's own window.
 *
 * This is the hinge of the port, and it replaces the SDL jump table the
 * Total Party Kill wrapper used.
 *
 * Poor Bunny's liblime.so carries SDL 2.30.12 built for Android with the
 * Dynamic API compiled OUT: every SDL_* export is a real function body and
 * SDL's internal calls go straight to them, so there is no table to swap and
 * SDL's Android video driver is what runs. That driver creates its GL surface
 * like this:
 *
 *     window = Android_JNI_GetNativeWindow();          // ANativeWindow*
 *     ANativeWindow_setBuffersGeometry(window, 0, 0, format);
 *     eglCreateWindowSurface(display, config, (EGLNativeWindowType)window, 0);
 *
 * devkitPro's switch-mesa defines EGLNativeWindowType as NWindow*. So the
 * ANativeWindow handle this file hands back IS the Switch's NWindow, and the
 * pointer SDL threads through to eglCreateWindowSurface arrives at mesa as
 * exactly the type mesa expects. Nothing has to translate it.
 *
 * What still has to be translated is the geometry call, which on Android
 * resizes the buffer queue and on the Switch is nwindowSetDimensions plus a
 * crop. Android's convention of (0, 0) meaning "keep the window's own size"
 * is honoured.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdint.h>
#include <string.h>
#include <EGL/egl.h>
#include <switch.h>

#include "android_stubs.h"
#include "config.h"
#include "log.h"
#include "nativewindow.h"
#include "nx_pointer.h"
#include "watchdog.h"

/* android/native_window.h */
#define WINDOW_FORMAT_RGBA_8888 1
#define WINDOW_FORMAT_RGBX_8888 2
#define WINDOW_FORMAT_RGB_565   4

static NWindow *g_win;
static int      g_w = 1280, g_h = 720;
static int      g_format = WINDOW_FORMAT_RGBA_8888;
static int      g_ready;

/* Docked output is up to 1080p, handheld is the 720p panel. "auto" follows
 * the dock; a fixed resolution is honoured in both modes. */
static void wanted_size(int *w, int *h)
{
    if (g_cfg.resolution == PB_RES_720) {
        *w = 1280; *h = 720;
        return;
    }
    if (g_cfg.resolution == PB_RES_1080) {
        *w = 1920; *h = 1080;
        return;
    }
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        *w = 1920; *h = 1080;
    } else {
        *w = 1280; *h = 720;
    }
}

int pbnw_init(void)
{
    int w, h;

    g_win = nwindowGetDefault();
    if (!g_win) {
        LOGE("nativewindow: nwindowGetDefault() returned nothing");
        return 0;
    }
    wanted_size(&w, &h);
    if (!pbnw_resize(w, h))
        return 0;
    g_ready = 1;
    LOGI("nativewindow: %dx%d (%s)", g_w, g_h,
         g_cfg.resolution == PB_RES_AUTO ? "auto, follows the dock" : "fixed");
    return 1;
}

int pbnw_resize(int w, int h)
{
    Result rc;

    if (!g_win || w <= 0 || h <= 0)
        return 0;
    rc = nwindowSetDimensions(g_win, (u32)w, (u32)h);
    if (R_FAILED(rc)) {
        LOGE("nativewindow: nwindowSetDimensions(%d, %d) failed 0x%x", w, h, rc);
        return 0;
    }
    rc = nwindowSetCrop(g_win, 0, 0, (u32)w, (u32)h);
    if (R_FAILED(rc))
        LOGI("nativewindow: nwindowSetCrop failed 0x%x (harmless on most builds)", rc);
    g_w = w;
    g_h = h;
    return 1;
}

/* Called from the dock/undock handler. Returns 1 when the size changed, which
 * is the signal to tell SDL its surface moved. */
int pbnw_follow_operation_mode(void)
{
    int w, h;

    if (g_cfg.resolution != PB_RES_AUTO)
        return 0;
    wanted_size(&w, &h);
    if (w == g_w && h == g_h)
        return 0;
    if (!pbnw_resize(w, h))
        return 0;
    LOGI("nativewindow: output is now %dx%d", g_w, g_h);
    return 1;
}

void *pbnw_handle(void)   { return g_win; }
int   pbnw_width(void)    { return g_w; }
int   pbnw_height(void)   { return g_h; }
int   pbnw_is_ready(void) { return g_ready; }

/* ------------------------------------------ the imported ANativeWindow --- */

/* SDL calls this with the Surface object our JNI layer returned from
 * SDLActivity.getNativeSurface(). The object carries nothing: the Switch has
 * exactly one window and this file owns it. */
void *ax_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    if (!g_win)
        LOG_ONCE("!! ANativeWindow_fromSurface before pbnw_init()");
    return g_win;
}

void ax_ANativeWindow_acquire(void *w)
{
    (void)w;   /* the one window is never freed, so references need no count */
}

void ax_ANativeWindow_release(void *w)
{
    (void)w;   /* libnx owns the default window; it outlives every caller */
}

int ax_ANativeWindow_getWidth(void *w)  { (void)w; return g_w; }
int ax_ANativeWindow_getHeight(void *w) { (void)w; return g_h; }
int ax_ANativeWindow_getFormat(void *w) { (void)w; return g_format; }

int ax_ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt)
{
    (void)w;
    /* Android: 0 means "whatever the window already is". SDL passes 0, 0 and
     * only sets the format, which is what makes this a no-op in the common
     * case. */
    if (fmt)
        g_format = fmt;
    if (width > 0 && height > 0 && (width != g_w || height != g_h)) {
        LOGI("nativewindow: the game asked for %dx%d", width, height);
        if (!pbnw_resize(width, height))
            return -1;   /* -EINVAL-ish; SDL treats any negative as failure */
    }
    return 0;
}

/* ------------------------------------------------- the one GL surface ---
 * libnx's single NWindow hosts exactly one swapchain. A second
 * eglCreateWindowSurface against it corrupts the buffer queue and the next
 * present faults the vi compositor -- learned from a working Switch port of
 * a Unity title, which hit it when the engine recreated its graphics state.
 *
 * SDL does the same thing on this port: the dock/undock path calls
 * onNativeSurfaceDestroyed and then onNativeSurfaceChanged, and SDL's Android
 * video driver destroys and rebuilds its EGL surface across that pair.
 *
 * So the surface is a singleton. The first create is real, every later one
 * for the same native window returns the same EGLSurface, and destroying it
 * is a no-op. SDL is none the wiser: it holds a handle that stays valid.
 *
 * SDL reaches EGL by dlopen()ing libEGL.so and dlsym()ing each entry point,
 * which dl_bridge.c answers from imports.c -- so these two wrappers are all
 * it takes to interpose.
 */
static EGLSurface g_gl_surface;
static void      *g_gl_native;

EGLSurface pbnw_egl_create_window_surface(EGLDisplay dpy, EGLConfig cfg,
                                          void *native, const EGLint *attribs)
{
    EGLSurface s;

    if (g_gl_surface && g_gl_native == native) {
        LOG_ONCE("gl: reusing the one window surface (a second one would "
                 "corrupt the buffer queue)");
        return g_gl_surface;
    }
    s = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)native, attribs);
    if (s != EGL_NO_SURFACE) {
        g_gl_surface = s;
        g_gl_native = native;
        /* mesa builds its swapchain inside that call and resets the window's
         * crop and transform to its own defaults, so put ours back. */
        pbnw_reassert();
        pbnw_report("after eglCreateWindowSurface");
    } else {
        LOGE("gl: eglCreateWindowSurface failed (0x%x) -- this is the "
             "NWindow-as-ANativeWindow assumption failing", eglGetError());
    }
    return s;
}

EGLBoolean pbnw_egl_destroy_surface(EGLDisplay dpy, EGLSurface surface)
{
    if (surface && surface == g_gl_surface) {
        LOG_ONCE("gl: keeping the one window surface alive across a destroy");
        return EGL_TRUE;
    }
    return eglDestroySurface(dpy, surface);
}

/* Counting frames without owning a render loop: every presented frame goes
 * through here, and SDL reaches EGL through the import table. */
/* frame_stats = 1: slow frames (> 20 ms) and the worst frame, per window */
static u64      g_last_swap, g_worst_us;
static unsigned g_slow;

void pbnw_frame_stats(unsigned *slow, unsigned *worst_ms)
{
    *slow = g_slow;
    *worst_ms = (unsigned)(g_worst_us / 1000);
    g_slow = 0;
    g_worst_us = 0;
}

EGLBoolean pbnw_egl_swap_buffers(EGLDisplay dpy, EGLSurface surface)
{
    EGLBoolean ok;

    /* The game's context is current on this thread and the frame is finished,
     * so this is the one moment the cursor can be drawn over it. */
    nxp_draw();

    ok = eglSwapBuffers(dpy, surface);
    if (ok) {
        if (g_cfg.frame_stats) {
            u64 now = armGetSystemTick();
            if (g_last_swap) {
                u64 us = armTicksToNs(now - g_last_swap) / 1000;
                if (us > 20000) g_slow++;
                if (us > g_worst_us) g_worst_us = us;
            }
            g_last_swap = now;
        }
        watchdog_note_swap();
        if (watchdog_swaps() == 1) {
            LOGI("gl: the first frame has been presented");
            pbnw_report("after the first frame");
        }
    } else {
        LOG_ONCE("!! gl: eglSwapBuffers failed (0x%x)", eglGetError());
    }
    return ok;
}

void pbnw_reassert(void)
{
    if (!g_win)
        return;
    nwindowSetCrop(g_win, 0, 0, (u32)g_w, (u32)g_h);
    nwindowSetTransform(g_win, 0u);
}

/* Everything here is a plain struct read, no syscalls. Worth having: it is
 * the only direct evidence of what mesa actually built. */
void pbnw_report(const char *when)
{
    unsigned n = 0;
    u64 m;

    if (!g_win)
        return;
    for (m = g_win->slots_configured; m; m >>= 1)
        n += (unsigned)(m & 1);
    LOGI("gl: nwindow %s: buffers=%u cur_slot=%d swap_interval=%u "
         "consumer_behind=%d %ux%u",
         when ? when : "", n, (int)g_win->cur_slot,
         (unsigned)g_win->swap_interval, (int)g_win->consumer_running_behind,
         g_win->width, g_win->height);
}

/* EGL 1.5's platform-display entry point. SDL loads it only when EGL reports
 * exactly 1.5, and treats a failure to find it as fatal -- so it has to be in
 * the table, but pointing at switch-mesa's own symbol would make the port
 * fail to LINK against any mesa that predates EGL 1.5.
 *
 * There is one display here, so the platform argument carries no information:
 * eglGetDisplay answers the same question. SDL falls back to exactly this
 * call anyway when the platform path yields EGL_NO_DISPLAY. */
EGLDisplay pbnw_egl_get_platform_display(unsigned int platform, void *native_display,
                                         const void *attrib_list)
{
    (void)platform; (void)attrib_list;
    return eglGetDisplay((EGLNativeDisplayType)native_display);
}

/* Defold's GLFW reads the window size with eglQuerySurface(EGL_WIDTH/HEIGHT)
 * straight after creating the surface. switch-mesa reports 0 until the first
 * buffer has been dequeued, so the engine sized its viewport 0x0 and drew
 * nothing but the clear colour. The window surface's size is the NWindow's,
 * so answer from here. */
EGLBoolean pbnw_egl_query_surface(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint *value)
{
    EGLBoolean ok = eglQuerySurface(dpy, surface, attr, value);
    if (surface && surface == g_gl_surface && value && (attr == EGL_WIDTH || attr == EGL_HEIGHT)) {
        EGLint want = attr == EGL_WIDTH ? g_w : g_h;
        if (!ok || *value != want)
            LOG_ONCE("gl: eglQuerySurface said %dx? for the window; answering %dx%d", ok ? *value : -1, g_w, g_h);
        *value = want;
        return EGL_TRUE;
    }
    return ok;
}
