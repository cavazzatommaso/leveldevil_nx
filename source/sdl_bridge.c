/* sdl_bridge.c -- see sdl_bridge.h. MIT licensed, see LICENSE. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_vulkan.h>

#include "app.h"
#include "config.h"
#include "input.h"
#include "jni_env.h"
#include "log.h"
#include "nx_pointer.h"
#include "paths.h"
#include "sdl_bridge.h"
#include "so_util.h"

/* Slot indices by name, from the generated header: SLOT_SDL_PollEvent etc. */
enum {
#define SDLP(i, name, kind) SLOT_##name = i,
#include "sdl_procs.h"
#undef SDLP
};

/* switch-mesa, linked statically; declared here to keep GL headers out */
extern const unsigned char *glGetString(unsigned int name);

static so_module  *g_lime;
static int            g_joystick_opened;
static SDL_JoystickID g_joystick_instance;
static int            g_gamepad_opened;
static int            g_mapping_installed;
static SDL_Window *g_window;

/* Called from the joystick query overrides: a query can only come from the
 * game's own code asking about the device, so it tells us the game is at
 * least aware of it. */
void sdlb_note_pad_query(const char *what)
{
    LOG_ONCE("input: the game inspected the pad (%s), so it is looking at it", what);
    (void)what;
}

int sdlb_game_opened_joystick(void) { return g_joystick_opened; }
int sdlb_game_opened_gamepad(void) { return g_gamepad_opened; }
SDL_JoystickID sdlb_joystick_instance(void) { return g_joystick_instance; }

SDL_Window *sdlb_window(void) { return g_window; }

/* ------------------------------------------------- Android-only entries -- */

static void *hs_SDL_AndroidGetJNIEnv(void) { return (void *)jni_get_env(); }
static void *hs_SDL_AndroidGetActivity(void) { return (void *)jni_ref(jni_activity()); }
static const char *hs_SDL_AndroidGetInternalStoragePath(void) { return paths_save_nodev(); }
static int hs_SDL_AndroidGetExternalStorageState(void) { return 3; }  /* read | write */
static const char *hs_SDL_AndroidGetExternalStoragePath(void) { return paths_save_nodev(); }
static SDL_bool hs_SDL_IsAndroidTV(void) { return SDL_FALSE; }
static SDL_bool hs_SDL_IsChromebook(void) { return SDL_FALSE; }
static SDL_bool hs_SDL_IsDeXMode(void) { return SDL_FALSE; }
static void hs_SDL_AndroidBackButton(void) {}
static int hs_SDL_GetAndroidSDKVersion(void) { return 29; }

/* ----------------------------------------------------------- helpers ---- */

static void output_size(int *w, int *h)
{
    if (g_cfg.resolution == HS_RES_1080 ||
        (g_cfg.resolution == HS_RES_AUTO && appletGetOperationMode() == AppletOperationMode_Console)) {
        *w = 1920;
        *h = 1080;
    } else {
        *w = 1280;
        *h = 720;
    }
}

static void current_size(int *w, int *h)
{
    if (g_window)
        SDL_GetWindowSize(g_window, w, h);
    else
        output_size(w, h);
}

static void SDLCALL sdl_log_output(void *ud, int category, SDL_LogPriority prio, const char *msg)
{
    (void)ud;
    if (prio < SDL_LOG_PRIORITY_INFO && log_get_level() < HS_LOG_DEBUG)
        return;
    log_printf("SDL[%d/%d]: %s", category, (int)prio, msg ? msg : "");
}

/* ----------------------------------------------------------- events ----- */

static SDL_EventFilter g_app_filter;
static void           *g_app_filter_ud;
static int             g_filter_installed;
static int             g_ctrl_logged;

static int SDLCALL event_filter(void *ud, SDL_Event *e)
{
    const Uint32 t = e->type;
    (void)ud;

    if (t >= SDL_CONTROLLERAXISMOTION && t < SDL_FINGERDOWN) {
        if (!g_cfg.gamepad)
            return 0;
        /* Proof that SDL's controller layer is producing input from the real
         * pad and handing it to the game. */
        if (g_ctrl_logged < 8) {
            g_ctrl_logged++;
            if (t == SDL_CONTROLLERBUTTONDOWN || t == SDL_CONTROLLERBUTTONUP)
                LOGI("controller event: button %d %s (device %d)", e->cbutton.button,
                     t == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", (int)e->cbutton.which);
            else if (t == SDL_CONTROLLERAXISMOTION)
                LOGI("controller event: axis %d = %d (device %d)", e->caxis.axis,
                     e->caxis.value, (int)e->caxis.which);
            else
                LOGI("controller event: type 0x%x (device %d)", t, (int)e->cdevice.which);
        }
    } else if (!input_is_injecting()) {
        if (t >= SDL_JOYAXISMOTION && t < SDL_CONTROLLERAXISMOTION) {
            /* SDL polls the physical pad itself for the joystick the game
             * opened; seeing these confirms that half of the chain works. */
            if (t == SDL_JOYBUTTONDOWN)
                LOG_ONCE("input: SDL is reading the physical pad (its own joystick events "
                         "are dropped in favour of the synthesized ones)");
            return 0;                                   /* joystick: input.c owns these */
        }
        if (t >= SDL_FINGERDOWN && t <= SDL_FINGERMOTION)
            return 0;                                   /* touch: input.c owns these */
        if (t >= SDL_DOLLARGESTURE && t <= SDL_MULTIGESTURE)
            return 0;
        if ((t == SDL_MOUSEMOTION && e->motion.which == SDL_TOUCH_MOUSEID) ||
            ((t == SDL_MOUSEBUTTONDOWN || t == SDL_MOUSEBUTTONUP) && e->button.which == SDL_TOUCH_MOUSEID))
            return 0;
    }
    if (t == SDL_QUIT) {
        LOGI("SDL_QUIT delivered to the game");
        hs_note_quit();
    }
    else if (t == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
        LOGI("window size changed to %dx%d", e->window.data1, e->window.data2);
    return g_app_filter ? g_app_filter(g_app_filter_ud, e) : 1;
}

/* switch-sdl2 ships a mapping for its pad, but a positional one: its
 * "A" is the bottom button. Replace it so the controller path agrees with
 * button_layout -- with the default, the game's A is the Switch's A.
 *
 * Native button order is libnx's: 0 A, 1 B, 2 X, 3 Y, 4/5 stick clicks,
 * 6 L, 7 R, 8 ZL, 9 ZR, 10 Plus, 11 Minus, 12..15 D-pad left/up/right/down. */
static void install_controller_mapping(void)
{
    char guid[64], mapping[512];
    const int label = g_cfg.button_layout == HS_LAYOUT_LABEL;
    int rc;

    if (g_mapping_installed || !SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        return;
    g_mapping_installed = 1;
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(0), guid, sizeof(guid));
    snprintf(mapping, sizeof(mapping),
             "%s,Switch Controller,"
             "a:b%d,b:b%d,x:b%d,y:b%d,"
             "back:b11,start:b10,leftshoulder:b6,rightshoulder:b7,"
             "lefttrigger:b8,righttrigger:b9,leftstick:b4,rightstick:b5,"
             "dpup:b13,dpdown:b15,dpleft:b12,dpright:b14,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,",
             guid, label ? 0 : 1, label ? 1 : 0, label ? 2 : 3, label ? 3 : 2);
    rc = SDL_GameControllerAddMapping(mapping);
    LOGI("input: controller mapping for %s (%s layout): %s", guid,
         label ? "label" : "position",
         rc < 0 ? SDL_GetError() : (rc == 0 ? "updated" : "added"));
}

static void after_init(void)
{
    if (!g_filter_installed && SDL_WasInit(SDL_INIT_EVENTS)) {
        SDL_SetEventFilter(event_filter, NULL);
        g_filter_installed = 1;
    }
    install_controller_mapping();
    input_on_sdl_init();
}

static int ovr_SDL_Init(Uint32 flags)
{
    int r;
    LOGI("SDL_Init(0x%x)", flags);
    SDL_LogSetOutputFunction(sdl_log_output, NULL);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    r = SDL_Init(flags);
    if (r != 0)
        LOGE("SDL_Init failed: %s", SDL_GetError());
    after_init();
    return r;
}

static int ovr_SDL_InitSubSystem(Uint32 flags)
{
    int r = SDL_InitSubSystem(flags);
    LOGI("SDL_InitSubSystem(0x%x) = %d", flags, r);
    after_init();
    return r;
}

static void ovr_SDL_Quit(void)
{
    LOGI("SDL_Quit");
    log_flush();
    SDL_Quit();
}

static void ovr_SDL_SetEventFilter(SDL_EventFilter filter, void *ud)
{
    g_app_filter = filter;
    g_app_filter_ud = ud;
    if (!g_filter_installed) {
        SDL_SetEventFilter(event_filter, NULL);
        g_filter_installed = 1;
    }
}

static SDL_bool ovr_SDL_GetEventFilter(SDL_EventFilter *filter, void **ud)
{
    if (filter)
        *filter = g_app_filter;
    if (ud)
        *ud = g_app_filter_ud;
    return g_app_filter ? SDL_TRUE : SDL_FALSE;
}

static void ovr_SDL_PumpEvents(void)
{
    input_pump();
    SDL_PumpEvents();
}

static int ovr_SDL_PollEvent(SDL_Event *e)
{
    input_pump();
    return SDL_PollEvent(e);
}

static int ovr_SDL_PeepEvents(SDL_Event *events, int n, SDL_eventaction action, Uint32 min, Uint32 max)
{
    if (action != SDL_ADDEVENT)
        input_pump();
    return SDL_PeepEvents(events, n, action, min, max);
}

/* Waiting would sleep inside SDL without ever polling libnx, so wait by
 * polling instead. Lime's frame timer posts an event on schedule, so the
 * loop never spins long. */
static int ovr_SDL_WaitEventTimeout(SDL_Event *e, int timeout)
{
    const u64 start = armGetSystemTick();
    for (;;) {
        input_pump();
        if (SDL_PollEvent(e))
            return 1;
        if (timeout >= 0 && armTicksToNs(armGetSystemTick() - start) >= (u64)timeout * 1000000ULL)
            return 0;
        /* Lime waits here for the timer that paces its frames, so the sleep
         * granularity is frame jitter. A quarter of a millisecond costs very
         * little and keeps the wake-up close to the timer. */
        svcSleepThread(250000LL);
    }
}

static int ovr_SDL_WaitEvent(SDL_Event *e) { return ovr_SDL_WaitEventTimeout(e, -1); }

/* ----------------------------------------------------------- window ----- */

static SDL_Window *ovr_SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    SDL_Window *win;
    int ow, oh, msaa = 0;
    Uint32 nf = flags & ~(Uint32)(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    (void)x; (void)y;

    output_size(&ow, &oh);
    if (g_cfg.resolution == HS_RES_AUTO)
        nf |= SDL_WINDOW_RESIZABLE;
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &msaa);
    LOGI("SDL_CreateWindow('%s', %dx%d, flags 0x%x) -> %dx%d, flags 0x%x, msaa %d",
         title ? title : "", w, h, flags, ow, oh, nf, msaa);

    win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ow, oh, nf);
    if (!win && msaa > 0) {
        LOGI("window with %dx MSAA failed (%s), retrying without", msaa, SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ow, oh, nf);
    }
    if (!win)
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
    g_window = win;
    return win;
}

static SDL_GLContext ovr_SDL_GL_CreateContext(SDL_Window *win)
{
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        LOGE("SDL_GL_CreateContext failed: %s", SDL_GetError());
    } else {
        const unsigned char *vendor = glGetString(0x1F00), *renderer = glGetString(0x1F01),
                            *version = glGetString(0x1F02);
        LOGI("GL: %s | %s | %s", vendor ? (const char *)vendor : "?",
             renderer ? (const char *)renderer : "?", version ? (const char *)version : "?");
    }
    if (ctx && g_cfg.vsync) {
        /* SDL only ever calls eglSwapInterval. The presentation interval of
         * the window itself belongs to the display layer, and nothing sets it,
         * so frames can be handed over as soon as they are drawn no matter
         * what EGL was told. Setting it here is the part that is genuinely on
         * our side of the fence. */
        NWindow *nw = nwindowGetDefault();
        if (nw && R_SUCCEEDED(nwindowSetSwapInterval(nw, 1)))
            LOGI("vsync: window presentation interval 1, %d buffer(s) in use",
                 __builtin_popcountll(nw->slots_configured));
        else
            LOGI("vsync: could not set the window presentation interval");
        /* Apply it here as well: the game only sets the swap interval if its
         * project had vsync enabled, and without it frames are presented as
         * soon as they are drawn. */
        if (SDL_GL_SetSwapInterval(1) != 0)
            LOGI("vsync: SDL_GL_SetSwapInterval(1) refused (%s)", SDL_GetError());
        LOGI("vsync: swap interval is %d", SDL_GL_GetSwapInterval());
    }
    return ctx;
}

/* The Switch is always fullscreen, and switch-sdl2 has no SetWindowFullscreen
 * hook: letting SDL run its fullscreen path would only resize the window to a
 * display mode that does not match the panel. Report success and change
 * nothing. */
static int ovr_SDL_SetWindowFullscreen(SDL_Window *win, Uint32 flags)
{
    (void)win;
    LOGD("SDL_SetWindowFullscreen(0x%x) ignored (always fullscreen here)", flags);
    return 0;
}

/* switch-sdl2 performs the dock/undock resize only while the window is
 * resizable, so a game that turns resizability off would stay at 720p on the
 * TV. Keep the flag when the resolution is set to follow the dock. */
static void ovr_SDL_SetWindowResizable(SDL_Window *win, SDL_bool resizable)
{
    if (!resizable && g_cfg.resolution == HS_RES_AUTO) {
        LOGI("SDL_SetWindowResizable(false) ignored: it would stop docking from "
             "switching to 1080p (set resolution in config.txt to override)");
        return;
    }
    SDL_SetWindowResizable(win, resizable);
}

/* Lime does not call this, but if a future build does, a window sized to the
 * game's logical resolution would be upscaled by the panel instead of the
 * renderer. */
static void ovr_SDL_SetWindowSize(SDL_Window *win, int w, int h)
{
    int ow, oh;
    (void)win;
    output_size(&ow, &oh);
    LOGI("SDL_SetWindowSize(%d, %d) ignored; the output stays %dx%d", w, h, ow, oh);
}

/* Lime discards joystick events, including the "device added" one, until the
 * Haxe side has registered its joystick callback -- which happens long after
 * SDL_Init. input.c therefore re-announces the pad until this override sees
 * the game open it. */
static SDL_Joystick *ovr_SDL_JoystickOpen(int device_index)
{
    SDL_Joystick *j = SDL_JoystickOpen(device_index);
    if (!j) {
        LOGI("SDL_JoystickOpen(%d) failed: %s", device_index, SDL_GetError());
        return NULL;
    }
    if (device_index == 0) {
        g_joystick_instance = SDL_JoystickInstanceID(j);
        if (!g_joystick_opened) {
            g_joystick_opened = 1;
            LOGI("input: the game opened joystick 0 (instance %d) -- buttons and sticks are live",
                 (int)g_joystick_instance);
        }
    }
    return j;
}

/* The game may use the pad through SDL's game-controller layer instead (Lime
 * Gamepad -> OpenFL GameInput). That path is native: SDL polls the switch
 * joystick itself and maps it through the controller mapping installed below,
 * so all this has to do is notice that the game opened it. */
static SDL_GameController *ovr_SDL_GameControllerOpen(int joystick_index)
{
    SDL_GameController *c = SDL_GameControllerOpen(joystick_index);
    if (!c) {
        LOGI("SDL_GameControllerOpen(%d) failed: %s", joystick_index, SDL_GetError());
        return NULL;
    }
    if (joystick_index == 0 && !g_gamepad_opened) {
        g_gamepad_opened = 1;
        LOGI("input: the game opened gamepad 0 (\"%s\") -- the controller path is live",
             SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
    }
    return c;
}

static void ovr_SDL_DestroyWindow(SDL_Window *win)
{
    if (win == g_window)
        g_window = NULL;
    SDL_DestroyWindow(win);
}

static u64 g_stats_tick, g_last_swap, g_frame_ns, g_swap_ns, g_frame_worst;
static unsigned g_frames, g_frame_ontime, g_frame_slow, g_frame_stall;
static int g_first_frame;

/* Frame pacing. The interval between presents tells us whether the game is
 * actually landing on refreshes; the time spent inside the swap tells us
 * whether vsync is doing the waiting (a swap that always returns instantly
 * means frames are being presented whenever they happen to be ready, which
 * is what judder at a "stable 60" looks like). */
static void ovr_SDL_GL_SwapWindow(SDL_Window *win)
{
    const u64 now = armGetSystemTick();
    u64 swap_ns;

    if (!g_first_frame) {
        g_first_frame = 1;
        g_stats_tick = g_last_swap = now;
        LOGI("first frame presented (swap interval %d)", SDL_GL_GetSwapInterval());
    } else {
        u64 dt = armTicksToNs(now - g_last_swap);
        g_frames++;
        g_frame_ns += dt;
        if (dt > g_frame_worst)
            g_frame_worst = dt;
        if (dt < 18000000ULL)
            g_frame_ontime++;
        else if (dt < 34000000ULL)
            g_frame_slow++;            /* missed a refresh */
        else
            g_frame_stall++;           /* missed two or more */
    }
    g_last_swap = now;

    /* The cursor is drawn inside the game's context, right before the frame
     * goes out; nxp_draw saves and restores every piece of state it touches. */
    if (g_cfg.pointer && nxp_visible()) {
        int ww = 1280, wh = 720;
        SDL_GetWindowSize(win, &ww, &wh);
        nxp_set_screen(ww, wh);
        nxp_draw();
    }

    SDL_GL_SwapWindow(win);

    swap_ns = armTicksToNs(armGetSystemTick() - now);
    g_swap_ns += swap_ns;

    if (g_cfg.frame_stats && armTicksToNs(now - g_stats_tick) >= 5000000000ULL && g_frames) {
        double secs = (double)armTicksToNs(now - g_stats_tick) / 1e9;
        LOGI("frames: %.1f fps, avg %.2f ms, worst %.1f ms | on time %u, late %u, stalled %u "
             "| %.2f ms in swap",
             g_frames / secs, (double)g_frame_ns / g_frames / 1e6, (double)g_frame_worst / 1e6,
             g_frame_ontime, g_frame_slow, g_frame_stall, (double)g_swap_ns / g_frames / 1e6);
        g_frames = 0;
        g_frame_ns = g_swap_ns = g_frame_worst = 0;
        g_frame_ontime = g_frame_slow = g_frame_stall = 0;
        g_stats_tick = now;
    }
}

/* The game only asks for vsync when its project was built with it enabled.
 * Without it the frame timer (millisecond resolution) and the 60 Hz panel
 * drift, and motion judders even though the frame rate reads as 60. */
static int ovr_SDL_GL_SetSwapInterval(int interval)
{
    if (g_cfg.vsync && interval != 1) {
        LOGI("SDL_GL_SetSwapInterval(%d) -> forcing 1 (vsync = 0 in config.txt to allow %d)",
             interval, interval);
        interval = 1;
    }
    return SDL_GL_SetSwapInterval(interval);
}

static int ovr_SDL_GetDesktopDisplayMode(int index, SDL_DisplayMode *mode)
{
    int r = SDL_GetDesktopDisplayMode(index, mode);
    if (r == 0 && mode)
        current_size(&mode->w, &mode->h);
    return r;
}

static int ovr_SDL_GetCurrentDisplayMode(int index, SDL_DisplayMode *mode)
{
    int r = SDL_GetCurrentDisplayMode(index, mode);
    if (r == 0 && mode)
        current_size(&mode->w, &mode->h);
    return r;
}

static int ovr_SDL_GetDisplayMode(int index, int mode_index, SDL_DisplayMode *mode)
{
    int r = SDL_GetDisplayMode(index, mode_index, mode);
    if (r == 0 && mode)
        current_size(&mode->w, &mode->h);
    return r;
}

static int ovr_SDL_GetDisplayBounds(int index, SDL_Rect *rect)
{
    int r = SDL_GetDisplayBounds(index, rect);
    if (r == 0 && rect) {
        rect->x = rect->y = 0;
        current_size(&rect->w, &rect->h);
    }
    return r;
}

static int ovr_SDL_GetDisplayUsableBounds(int index, SDL_Rect *rect)
{
    return ovr_SDL_GetDisplayBounds(index, rect);
}

static int ovr_SDL_GetDisplayDPI(int index, float *ddpi, float *hdpi, float *vdpi)
{
    (void)index;
    if (ddpi) *ddpi = (float)g_cfg.dpi;
    if (hdpi) *hdpi = (float)g_cfg.dpi;
    if (vdpi) *vdpi = (float)g_cfg.dpi;
    return 0;
}

/* ------------------------------------------------------------ files ----- */

static char *ovr_SDL_GetBasePath(void)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/", paths_assets_nodev());
    return SDL_strdup(buf);
}

static char *ovr_SDL_GetPrefPath(const char *org, const char *app)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s/", paths_save_nodev());
    LOGI("SDL_GetPrefPath(%s, %s) -> %s", org ? org : "", app ? app : "", buf);
    mkdir(paths_save(), 0777);
    return SDL_strdup(buf);
}

static SDL_RWops *ovr_SDL_RWFromFile(const char *file, const char *mode)
{
    char p[1024];
    SDL_RWops *rw;
    path_translate(file, p, sizeof(p));
    rw = SDL_RWFromFile(p, mode);
    if (!rw)
        LOGI("SDL_RWFromFile(%s, %s) failed: %s", p, mode ? mode : "", SDL_GetError());
    else
        LOGD("SDL_RWFromFile(%s, %s)", p, mode ? mode : "");
    return rw;
}

/* --------------------------------------------------------- joystick ----- */

static int ovr_SDL_JoystickNumButtons(SDL_Joystick *j)
{
    sdlb_note_pad_query("button count");
    return input_is_virtual(j) ? input_num_buttons() : SDL_JoystickNumButtons(j);
}

static int ovr_SDL_JoystickNumAxes(SDL_Joystick *j)
{
    sdlb_note_pad_query("axis count");
    return input_is_virtual(j) ? input_num_axes() : SDL_JoystickNumAxes(j);
}

static int ovr_SDL_JoystickNumHats(SDL_Joystick *j)
{
    sdlb_note_pad_query("hat count");
    return input_is_virtual(j) ? input_num_hats() : SDL_JoystickNumHats(j);
}

static Uint8 ovr_SDL_JoystickGetButton(SDL_Joystick *j, int b)
{
    sdlb_note_pad_query("button state");
    return input_is_virtual(j) ? input_button(b) : SDL_JoystickGetButton(j, b);
}

static Sint16 ovr_SDL_JoystickGetAxis(SDL_Joystick *j, int a)
{
    sdlb_note_pad_query("axis value");
    return input_is_virtual(j) ? input_axis(a) : SDL_JoystickGetAxis(j, a);
}

static Uint8 ovr_SDL_JoystickGetHat(SDL_Joystick *j, int h)
{
    sdlb_note_pad_query("hat value");
    return input_is_virtual(j) ? input_hat(h) : SDL_JoystickGetHat(j, h);
}

/* ----------------------------------------------------------- misc ------- */

static int ovr_SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *w)
{
    (void)w;
    LOGI("message box (flags 0x%x): %s: %s", flags, title ? title : "", message ? message : "");
    log_flush();
    return 0;
}

static int ovr_SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
    if (data)
        LOGI("message box: %s: %s", data->title ? data->title : "", data->message ? data->message : "");
    if (buttonid)
        *buttonid = (data && data->numbuttons > 0) ? data->buttons[0].buttonid : -1;
    return 0;
}

static SDL_bool ovr_SDL_SetHint(const char *name, const char *value)
{
    LOGD("SDL_SetHint(%s, %s)", name ? name : "", value ? value : "");
    return SDL_SetHint(name, value);
}

static SDL_AudioDeviceID ovr_SDL_OpenAudioDevice(const char *device, int iscapture,
                                                 const SDL_AudioSpec *desired,
                                                 SDL_AudioSpec *obtained, int allowed_changes)
{
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
    if (desired)
        LOGI("SDL_OpenAudioDevice(%s): want %d Hz fmt 0x%x ch %d samples %d -> id %u%s%s",
             iscapture ? "capture" : "playback", desired->freq, desired->format, desired->channels,
             desired->samples, (unsigned)id, id ? "" : " error: ", id ? "" : SDL_GetError());
    return id;
}

/* ---------------------------------------------------- the jump table ---- */

static void *g_procs[SDL_PROC_COUNT];
static const char *g_names[SDL_PROC_COUNT];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static void build_procs(void)
{
#define SDLP_N(i, name) g_procs[i] = (void *)&name; g_names[i] = #name;
#define SDLP_V(i, name) g_procs[i] = (void *)&name; g_names[i] = #name;
#define SDLP_A(i, name) g_procs[i] = (void *)&hs_##name; g_names[i] = #name;
#define SDLP(i, name, kind) SDLP_##kind(i, name)
#include "sdl_procs.h"
#undef SDLP
#undef SDLP_N
#undef SDLP_V
#undef SDLP_A

    /* The table slots are void*, so nothing would otherwise catch an override
     * whose signature drifts from SDL's. Assigning it to a pointer of the real
     * function's type makes any mismatch a compile error. */
#define OVERRIDE(name)                                                    \
    do {                                                                  \
        __typeof__(&name) signature_must_match_ = &ovr_##name;            \
        (void)signature_must_match_;                                       \
        g_procs[SLOT_##name] = (void *)&ovr_##name;                        \
    } while (0)
    OVERRIDE(SDL_Init);
    OVERRIDE(SDL_InitSubSystem);
    OVERRIDE(SDL_Quit);
    OVERRIDE(SDL_SetEventFilter);
    OVERRIDE(SDL_GetEventFilter);
    OVERRIDE(SDL_PumpEvents);
    OVERRIDE(SDL_PollEvent);
    OVERRIDE(SDL_PeepEvents);
    OVERRIDE(SDL_WaitEvent);
    OVERRIDE(SDL_WaitEventTimeout);
    OVERRIDE(SDL_CreateWindow);
    OVERRIDE(SDL_DestroyWindow);
    OVERRIDE(SDL_SetWindowFullscreen);
    OVERRIDE(SDL_SetWindowResizable);
    OVERRIDE(SDL_SetWindowSize);
    OVERRIDE(SDL_JoystickOpen);
    OVERRIDE(SDL_GameControllerOpen);
    OVERRIDE(SDL_GL_CreateContext);
    OVERRIDE(SDL_GL_SwapWindow);
    OVERRIDE(SDL_GL_SetSwapInterval);
    OVERRIDE(SDL_GetDesktopDisplayMode);
    OVERRIDE(SDL_GetCurrentDisplayMode);
    OVERRIDE(SDL_GetDisplayMode);
    OVERRIDE(SDL_GetDisplayBounds);
    OVERRIDE(SDL_GetDisplayUsableBounds);
    OVERRIDE(SDL_GetDisplayDPI);
    OVERRIDE(SDL_GetBasePath);
    OVERRIDE(SDL_GetPrefPath);
    OVERRIDE(SDL_RWFromFile);
    OVERRIDE(SDL_JoystickNumButtons);
    OVERRIDE(SDL_JoystickNumAxes);
    OVERRIDE(SDL_JoystickNumHats);
    OVERRIDE(SDL_JoystickGetButton);
    OVERRIDE(SDL_JoystickGetAxis);
    OVERRIDE(SDL_JoystickGetHat);
    OVERRIDE(SDL_ShowSimpleMessageBox);
    OVERRIDE(SDL_ShowMessageBox);
    OVERRIDE(SDL_SetHint);
    OVERRIDE(SDL_OpenAudioDevice);
#undef OVERRIDE
}
#pragma GCC diagnostic pop

static void unmapped_slot(void)
{
    LOG_ONCE("SDL: call through an unmapped dynamic API slot");
}

/* Which table slot does this stub load? -1 if it does not look like one. */
static int decode_stub(uintptr_t fn, uint64_t size, uintptr_t table, size_t nslots)
{
    uintptr_t reg[32];
    uint32_t valid = 0;
    uint64_t k, n = size ? size : 32;
    if (n > 64 * 4)
        n = 64 * 4;
    for (k = 0; k + 4 <= n; k += 4) {
        uint32_t insn = *(uint32_t *)(fn + k);
        if ((insn & 0x9F000000u) == 0x90000000u) {               /* adrp */
            int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3));
            if (imm & (1 << 20))
                imm -= (1 << 21);
            reg[insn & 0x1F] = ((fn + k) & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
            valid |= 1u << (insn & 0x1F);
        } else if ((insn & 0xFFC00000u) == 0x91000000u) {        /* add x, x, #imm */
            unsigned rd = insn & 0x1F, rn = (insn >> 5) & 0x1F;
            uintptr_t imm = (uintptr_t)((insn >> 10) & 0xFFF) << (((insn >> 22) & 1) ? 12 : 0);
            if (valid & (1u << rn)) {
                reg[rd] = reg[rn] + imm;
                valid |= 1u << rd;
            }
        } else if ((insn & 0xFFC00000u) == 0xF9400000u) {        /* ldr x, [x, #imm] */
            unsigned rt = insn & 0x1F, rn = (insn >> 5) & 0x1F;
            if (valid & (1u << rn)) {
                uintptr_t addr = reg[rn] + ((uintptr_t)((insn >> 10) & 0xFFF) << 3);
                if (addr >= table && addr < table + nslots * 8 && !((addr - table) & 7))
                    return (int)((addr - table) / 8);
            }
            valid &= ~(1u << rt);
        }
    }
    return -1;
}

typedef struct {
    uintptr_t table;
    size_t    nslots;
    void    **dest;       /* non-NULL: fill by name */
    int       checked, mismatched, filled;
} StubScan;

static int find_name(const char *name)
{
    int i;
    for (i = 0; i < SDL_PROC_COUNT; i++)
        if (!strcmp(g_names[i], name))
            return i;
    return -1;
}

static int scan_stub(const char *name, uintptr_t addr, uint64_t size, int type, void *ud)
{
    StubScan *s = ud;
    int slot, idx;
    if (type != 2 || size != 32 || strncmp(name, "SDL_", 4) != 0)
        return 0;
    slot = decode_stub(addr, size, s->table, s->nslots);
    if (slot < 0)
        return 0;
    idx = find_name(name);
    if (idx < 0)
        return 0;
    s->checked++;
    if (s->dest) {
        s->dest[slot] = g_procs[idx];
        s->filled++;
    } else if (slot != idx) {
        if (s->mismatched++ < 10)
            LOGE("SDL: stub %s uses slot %d, expected %d", name, slot, idx);
    }
    return 0;
}

int32_t hs_SDL_DYNAPI_entry(uint32_t apiver, void *table, uint32_t tablesize)
{
    const size_t nslots = tablesize / sizeof(void *);
    StubScan scan;
    size_t i;

    LOGI("SDL dynamic API: version %u, %zu slots requested", apiver, nslots);
    if (apiver != 1 || !table) {
        LOGE("SDL dynamic API: unsupported version %u", apiver);
        return -1;
    }
    build_procs();

    memset(&scan, 0, sizeof(scan));
    scan.table = (uintptr_t)table;
    scan.nslots = nslots;
    if (g_lime)
        so_foreach_symbol(g_lime, scan_stub, &scan);

    if (nslots == SDL_PROC_COUNT && scan.checked >= 600 && scan.mismatched == 0) {
        memcpy(table, g_procs, sizeof(g_procs));
        LOGI("SDL dynamic API: %d stubs verified, table filled in order", scan.checked);
    } else {
        void **dest = table;
        for (i = 0; i < nslots; i++)
            dest[i] = (void *)&unmapped_slot;
        memset(&scan, 0, sizeof(scan));
        scan.table = (uintptr_t)table;
        scan.nslots = nslots;
        scan.dest = dest;
        if (g_lime)
            so_foreach_symbol(g_lime, scan_stub, &scan);
        LOGI("SDL dynamic API: order not verified (%zu slots, %d mismatches); filled %d slots by name",
             nslots, scan.mismatched, scan.filled);
    }
    return 0;
}

void sdlb_init(so_module *lime)
{
    g_lime = lime;
}
