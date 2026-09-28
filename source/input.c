/* input.c -- see input.h. MIT licensed, see LICENSE. */
#include <math.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "nx_pointer.h"
#include "sdl_bridge.h"

enum { XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_BACK, XB_START, XB_LS, XB_RS, XB_GUIDE, XB_COUNT };
enum { AX_LX, AX_LY, AX_LT, AX_RX, AX_RY, AX_RT, AX_COUNT };

#define JOY_DEVICE     0        /* device index announced to the game */
#define ANNOUNCE_MS    1000     /* re-announce until the game opens it */
#define ANNOUNCE_LIMIT 60       /* give up (and say so) after a minute */
#define TOUCH_DEVICE   1
#define MAX_FINGERS    16
#define COMBO_MS       2000
#define COMBO_FORCE_MS 6000

static PadState g_pad;
static int      g_ready;
static int      g_announced;
static u64      g_announce_at;
static u64      g_last_pump;
static int      g_announce_count;
static int      g_reconnects_done;
static u64      g_taken_at;
static int      g_keyboard_active;
static int      g_chord_held;
static u64      g_pointer_at;
static __thread int t_injecting;

static Uint8  g_btn[XB_COUNT];
static Sint16 g_axis[AX_COUNT];
static Uint8  g_hat;

typedef struct { s32 id; float x, y; } Finger;
static Finger g_fingers[MAX_FINGERS];
static int    g_nfingers;
static s32    g_mouse_finger = -1;

static u64 g_combo_since;
static int g_combo_fired;
static u64 g_combo_fired_at;
static int g_back_held;

static SDL_Joystick *g_rumble_joy;

int input_is_injecting(void) { return t_injecting; }

/* Queue the event directly instead of going through SDL_PushEvent.
 *
 * SDL_PushEvent runs the event watchers, and the game-controller subsystem
 * installs one that inspects every joystick event for an instance it has a
 * controller open on. This pad is described to the game as an XInput-shaped
 * device (1 hat, 6 axes), while the joystick SDL bound that controller to
 * reports 0 hats and 4 axes -- so the watcher indexed a zero-length hat array
 * and read through NULL. SDL_PeepEvents(SDL_ADDEVENT) adds to the queue with
 * no filter and no watchers, which is exactly right here: these events are
 * for the game, not for SDL's own controller layer, which polls the real
 * device by itself. */
static void push(SDL_Event *e)
{
    e->common.timestamp = SDL_GetTicks();
    t_injecting = 1;
    if (SDL_PeepEvents(e, 1, SDL_ADDEVENT, 0, 0) <= 0)
        LOG_ONCE("input: event queue full, input event dropped (%s)", SDL_GetError());
    t_injecting = 0;
}

static u64 now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

/* The id the game knows the pad by: the instance it got back from
 * SDL_JoystickOpen, or the device index until it has opened anything. */
static void release_all(void);
static void keys_release_all(void);
static void touch_mouse(Uint32 type, float nx, float ny, int dx, int dy);
static void finger_event(Uint32 type, s32 id, float x, float y, float dx, float dy);
static void stick(s32 x, s32 y, Sint16 *ox, Sint16 *oy);

static SDL_JoystickID joy_id(void)
{
    return sdlb_game_opened_joystick() ? sdlb_joystick_instance() : JOY_DEVICE;
}

void input_init(void)
{
    int i;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    g_ready = 1;
    LOGI("input: the pad sends the game's keys (%s), deadzone %d, touch %s%s",
         g_cfg.keys ? "on" : "OFF", g_cfg.stick_deadzone,
         g_cfg.touch ? "on" : "off",
         g_cfg.touch && g_cfg.touch_mouse ? " (+mouse)" : "");
    for (i = 0; i < IN_COUNT; i++)
        if (g_cfg.map[i] != CTL_NONE)
            LOGI("input:   %-12s -> %s", config_input_name(i),
                 config_control_name(g_cfg.map[i]));
}

/* Lime discards joystick events -- including "device added" -- until the
 * game's Haxe code has registered its joystick callback, which happens well
 * after SDL_Init. A single announcement at startup is therefore lost, and
 * switch-sdl2 never sends another one, so the game would never learn the pad
 * exists. Keep announcing until it opens it. */
static void announce_joystick(void)
{
    SDL_Event e;
    int want_joystick, want_gamepad;
    u64 t;

    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
        return;

    /* Two ways in, because Stencyl builds differ: as a plain joystick (Lime
     * Joystick) and as a game controller (Lime Gamepad, which is what OpenFL's
     * GameInput is built on). Each is announced until the game opens it. */
    want_joystick = !sdlb_game_opened_joystick();
    want_gamepad = g_cfg.gamepad && !sdlb_game_opened_gamepad() &&
                   SDL_WasInit(SDL_INIT_GAMECONTROLLER);
    if (want_gamepad)
        LOG_ONCE("input: SDL %s this pad a game controller",
                 SDL_IsGameController(JOY_DEVICE) ? "recognises" : "does NOT recognise");
    else if (!sdlb_game_opened_gamepad())
        LOG_ONCE("input: not offering a game controller (%s)",
                 g_cfg.gamepad ? "SDL's controller subsystem is not initialised"
                               : "gamepad = 0 in config.txt");
    if ((!want_joystick && !want_gamepad) || g_announce_count > ANNOUNCE_LIMIT)
        return;

    t = now_ms();
    if (g_announced && t - g_announce_at < ANNOUNCE_MS)
        return;
    g_announce_at = t;

    if (want_joystick) {
        memset(&e, 0, sizeof(e));
        e.type = SDL_JOYDEVICEADDED;
        e.jdevice.which = JOY_DEVICE;
        push(&e);
    }
    if (want_gamepad) {
        memset(&e, 0, sizeof(e));
        e.type = SDL_CONTROLLERDEVICEADDED;
        e.cdevice.which = JOY_DEVICE;
        push(&e);
    }

    if (!g_announced) {
        g_announced = 1;
        LOGI("input: offered the pad to the game as joystick %d%s "
             "(a courtesy: this build has no joystick handler)", JOY_DEVICE,
             want_gamepad ? " and as a game controller" : "");
    } else if (++g_announce_count == 5) {
        LOGI("input: the game has not taken the pad yet (joystick %s, controller %s); "
             "still offering it", want_joystick ? "no" : "yes",
             want_gamepad ? "no" : (g_cfg.gamepad ? "yes" : "off"));
    } else if (g_announce_count == ANNOUNCE_LIMIT) {
        /* Expected: this build has no joystick handler. The controls come from
         * the key mapping, so this is a note, not a failure. */
        LOGI("input: the game never opened the pad, which is what a build with no "
             "joystick handler does; the pad works through the key mapping");
    }
}

/* A game can only notice a controller that connects while it is listening.
 * Lime takes the pad during its own start-up, long before Stencyl's engine
 * sets up input, and each connection is announced exactly once -- so a
 * listener that registers later never learns the pad exists. Disconnect and
 * reconnect it a few times over the first half minute to give late listeners
 * a connection of their own. */
static void reconnect_pad(void)
{
    static const u64 schedule_ms[] = { 3000, 8000, 16000, 28000 };
    SDL_Event e;
    u64 t;

    if (g_reconnects_done >= g_cfg.pad_reconnects ||
        g_reconnects_done >= (int)(sizeof(schedule_ms) / sizeof(schedule_ms[0])))
        return;
    if (!sdlb_game_opened_joystick() && !sdlb_game_opened_gamepad())
        return;                                  /* nothing has taken it yet */
    if (!g_taken_at)
        g_taken_at = now_ms();
    t = now_ms();
    if (t - g_taken_at < schedule_ms[g_reconnects_done])
        return;
    g_reconnects_done++;

    if (sdlb_game_opened_joystick()) {
        memset(&e, 0, sizeof(e));
        e.type = SDL_JOYDEVICEREMOVED;
        e.jdevice.which = (Sint32)sdlb_joystick_instance();
        push(&e);
        memset(&e, 0, sizeof(e));
        e.type = SDL_JOYDEVICEADDED;
        e.jdevice.which = JOY_DEVICE;
        push(&e);
    }
    if (sdlb_game_opened_gamepad()) {
        memset(&e, 0, sizeof(e));
        e.type = SDL_CONTROLLERDEVICEREMOVED;
        e.cdevice.which = (Sint32)sdlb_joystick_instance();
        push(&e);
        memset(&e, 0, sizeof(e));
        e.type = SDL_CONTROLLERDEVICEADDED;
        e.cdevice.which = JOY_DEVICE;
        push(&e);
    }
    release_all();
    LOGI("input: reconnected the pad (%d of %d) so a listener that started late sees it",
         g_reconnects_done, g_cfg.pad_reconnects);
}

void input_on_sdl_init(void)
{
    announce_joystick();
}

/* ---------------------------------------------------------------- pad -- */

static void set_button(int b, int down)
{
    SDL_Event e;
    if (g_btn[b] == (Uint8)down)
        return;
    g_btn[b] = (Uint8)down;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
    e.jbutton.which = joy_id();
    e.jbutton.button = (Uint8)b;
    e.jbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
    push(&e);
    if (down)
        LOG_ONCE("input: first button press delivered (button %d, joystick %d)", b, (int)joy_id());
    LOGD("input: button %d %s", b, down ? "down" : "up");
}

static void set_axis(int a, Sint16 v)
{
    SDL_Event e;
    if (g_axis[a] == v)
        return;
    g_axis[a] = v;
    memset(&e, 0, sizeof(e));
    e.type = SDL_JOYAXISMOTION;
    e.jaxis.which = joy_id();
    e.jaxis.axis = (Uint8)a;
    e.jaxis.value = v;
    push(&e);
}

static void set_hat(Uint8 v)
{
    SDL_Event e;
    if (g_hat == v)
        return;
    g_hat = v;
    memset(&e, 0, sizeof(e));
    e.type = SDL_JOYHATMOTION;
    e.jhat.which = joy_id();
    e.jhat.hat = 0;
    e.jhat.value = v;
    push(&e);
    LOGD("input: hat 0x%x", v);
}

static void stick(s32 x, s32 y, Sint16 *ox, Sint16 *oy)
{
    const double dz = (double)g_cfg.stick_deadzone;
    double mag = sqrt((double)x * x + (double)y * y), scale, sx, sy;
    if (mag <= dz || mag < 1.0) {
        *ox = *oy = 0;
        return;
    }
    scale = (mag - dz) / (32767.0 - dz);
    if (scale > 1.0)
        scale = 1.0;
    scale *= 32767.0 / mag;
    sx = x * scale;
    sy = -y * scale;                        /* libnx: up is positive; SDL: down */
    *ox = (Sint16)(sx > 32767 ? 32767 : (sx < -32767 ? -32767 : sx));
    *oy = (Sint16)(sy > 32767 ? 32767 : (sy < -32767 ? -32767 : sy));
}

static void update_pad(u64 held)
{
    const int label = g_cfg.button_layout == HS_LAYOUT_LABEL;
    HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
    HidAnalogStickState rs = padGetStickPos(&g_pad, 1);
    Sint16 lx, ly, rx, ry;
    Uint8 hat = 0;

    set_button(label ? XB_A : XB_B, (held & HidNpadButton_A) != 0);
    set_button(label ? XB_B : XB_A, (held & HidNpadButton_B) != 0);
    set_button(label ? XB_X : XB_Y, (held & HidNpadButton_X) != 0);
    set_button(label ? XB_Y : XB_X, (held & HidNpadButton_Y) != 0);
    set_button(XB_LB, (held & HidNpadButton_L) != 0);
    set_button(XB_RB, (held & HidNpadButton_R) != 0);
    set_button(XB_BACK, (held & HidNpadButton_Minus) != 0);
    set_button(XB_START, (held & HidNpadButton_Plus) != 0);
    set_button(XB_LS, (held & HidNpadButton_StickL) != 0);
    set_button(XB_RS, (held & HidNpadButton_StickR) != 0);

    stick(ls.x, ls.y, &lx, &ly);
    stick(rs.x, rs.y, &rx, &ry);

    if (held & HidNpadButton_Up)    hat |= SDL_HAT_UP;
    if (held & HidNpadButton_Down)  hat |= SDL_HAT_DOWN;
    if (held & HidNpadButton_Left)  hat |= SDL_HAT_LEFT;
    if (held & HidNpadButton_Right) hat |= SDL_HAT_RIGHT;

    if (g_cfg.dpad != HS_DPAD_HAT) {
        if (hat & SDL_HAT_LEFT)  lx = -32767;
        if (hat & SDL_HAT_RIGHT) lx = 32767;
        if (hat & SDL_HAT_UP)    ly = -32767;
        if (hat & SDL_HAT_DOWN)  ly = 32767;
    }

    set_axis(AX_LX, lx);
    set_axis(AX_LY, ly);
    set_axis(AX_RX, rx);
    set_axis(AX_RY, ry);
    set_axis(AX_LT, (held & HidNpadButton_ZL) ? -32767 : 0);
    set_axis(AX_RT, (held & HidNpadButton_ZR) ? -32767 : 0);
    set_hat(g_cfg.dpad == HS_DPAD_STICK ? 0 : hat);
}

static void update_combo(u64 held)
{
    const u64 both = HidNpadButton_Plus | HidNpadButton_Minus;
    u64 t = now_ms();

    if (g_combo_fired) {
        if (t - g_combo_fired_at > COMBO_FORCE_MS && !hs_exit_requested()) {
            LOGI("input: game did not quit after SDL_QUIT, exiting");
            hs_request_exit(0);
        }
        return;
    }
    if (!g_cfg.exit_combo || (held & both) != both) {
        g_combo_since = 0;
        return;
    }
    if (!g_combo_since) {
        g_combo_since = t;
    } else if (t - g_combo_since >= COMBO_MS) {
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.type = SDL_QUIT;
        push(&e);
        g_combo_fired = 1;
        g_combo_fired_at = t;
        LOGI("input: + and - held, quitting");
    }
}

static void update_back_key(u64 held)
{
    int down = (held & HidNpadButton_Minus) != 0;
    SDL_Window *w;
    SDL_Event e;
    if (!g_cfg.minus_back_key || down == g_back_held)
        return;
    g_back_held = down;
    w = sdlb_window();
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = w ? SDL_GetWindowID(w) : 0;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = SDL_SCANCODE_AC_BACK;
    e.key.keysym.sym = SDLK_AC_BACK;
    push(&e);
}

/* ---------------------------------------------- pad as keyboard input -- */
/* This build of the game reads a keyboard, a mouse and touch -- never a
 * controller. Its com.stencyl.Input registers keyDown, keyUp, mouseDown,
 * mouseUp, touchBegin/Move/End and a swipe gesture, and carries no joystick
 * state at all; the "Gamepad Translator" tables in the binary belong to the
 * options menu's presentation, not to an input path. What it does have is a
 * complete keyboard control map, which the game ships inside itself as
 * config/game-config.json -- so the pad presses those keys.
 *
 * That is why this port never needs to know where the game draws its
 * on-screen buttons: the same controls drive gameplay, the title screen, the
 * level select, the options menu and the credits, and they keep working when
 * the player turns the on-screen controls off in Options.
 *
 * The keys below are the game's own defaults, read out of libApplicationMain.so
 * (PORTING.md says how). They are fixed, because they describe the game rather
 * than a preference; which Switch input presses which control is config.txt. */

static const struct { SDL_Scancode sc; SDL_Keycode sym; } CTL_KEY[] = {
    { SDL_SCANCODE_UNKNOWN, SDLK_UNKNOWN },   /* CTL_NONE       */
    { SDL_SCANCODE_LEFT,    SDLK_LEFT    },   /* CTL_LEFT       */
    { SDL_SCANCODE_RIGHT,   SDLK_RIGHT   },   /* CTL_RIGHT      */
    { SDL_SCANCODE_UP,      SDLK_UP      },   /* CTL_UP         */
    { SDL_SCANCODE_DOWN,    SDLK_DOWN    },   /* CTL_DOWN       */
    { SDL_SCANCODE_SPACE,   SDLK_SPACE   },   /* CTL_JUMP       */
    { SDL_SCANCODE_Z,       SDLK_z       },   /* CTL_ACTION1    */
    { SDL_SCANCODE_RETURN,  SDLK_RETURN  },   /* CTL_ENTER      */
    { SDL_SCANCODE_ESCAPE,  SDLK_ESCAPE  },   /* CTL_ESCAPE     */
    { SDL_SCANCODE_R,       SDLK_r       },   /* CTL_RESTART    */
    { SDL_SCANCODE_X,       SDLK_x       },   /* CTL_ACTION2    */
};

_Static_assert(sizeof CTL_KEY / sizeof CTL_KEY[0] == CTL_COUNT,
               "CTL_KEY is out of step with the CTL_* enum");

#define PTR_SLOT       0            /* the cursor is the only synthetic finger */
#define SYNTH_SLOTS    1
#define SYNTH_FINGER   100          /* an id well clear of the real fingers */
/* Deflection that counts as a direction, measured on the value stick() has
 * already rescaled past the dead zone -- so with the default dead zone of
 * 7000 this is about 40% of the physical travel. High enough that a resting
 * thumb or a worn stick never walks the level-select cursor, low enough to
 * hold a run comfortably. */
#define STICK_PRESS    8000

static Uint8 g_key_down[CTL_COUNT];
static u64   g_key_at[CTL_COUNT];

/* Lime reads keysym.sym and windowID; a key event with no window never
 * reaches the game. */
static void send_key(int ctl, int down, int repeat)
{
    SDL_Window *w = sdlb_window();
    SDL_Event e;

    if (CTL_KEY[ctl].sym == SDLK_UNKNOWN)
        return;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = w ? SDL_GetWindowID(w) : 0;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = (Uint8)repeat;
    e.key.keysym.scancode = CTL_KEY[ctl].sc;
    e.key.keysym.sym = CTL_KEY[ctl].sym;
    e.key.keysym.mod = KMOD_NONE;
    push(&e);
    if (down && !repeat)
        LOG_ONCE("input: the pad presses the game's keys (first: %s)",
                 config_control_name(ctl));
    LOGD("input: key %s %s", config_control_name(ctl), down ? "down" : "up");
}

static void set_key(int ctl, int down, u64 now)
{
    if (ctl <= CTL_NONE || ctl >= CTL_COUNT)
        return;
    if (down && !g_key_down[ctl]) {
        g_key_down[ctl] = 1;
        g_key_at[ctl] = now;
        send_key(ctl, 1, 0);
    } else if (!down && g_key_down[ctl]) {
        g_key_down[ctl] = 0;
        send_key(ctl, 0, 0);
    } else if (down && g_cfg.key_repeat_ms > 0 &&
               now - g_key_at[ctl] >= (u64)g_cfg.key_repeat_ms) {
        /* Off by default: the game's menus step once per press, and a repeat
         * during play would re-trigger a jump. */
        g_key_at[ctl] = now;
        send_key(ctl, 1, 1);
    }
}

static void keys_release_all(void)
{
    int c;
    for (c = CTL_NONE + 1; c < CTL_COUNT; c++) {
        if (g_key_down[c]) {
            g_key_down[c] = 0;
            send_key(c, 0, 0);
        }
    }
}

typedef struct { int down; float x, y; } SynthFinger;
static SynthFinger g_synth[SYNTH_SLOTS];

/* Press, move or lift one synthesized finger, with the same mouse events SDL
 * produced on Android for the first finger down. */
static void synth_touch(int slot, int down, float x, float y)
{
    SynthFinger *f = &g_synth[slot];
    const s32 id = SYNTH_FINGER + slot;

    if (down && !f->down) {
        f->down = 1;
        f->x = x;
        f->y = y;
        if (g_cfg.touch_mouse && g_mouse_finger < 0) {
            g_mouse_finger = id;
            touch_mouse(SDL_MOUSEMOTION, x, y, 0, 0);
            touch_mouse(SDL_MOUSEBUTTONDOWN, x, y, 0, 0);
        }
        finger_event(SDL_FINGERDOWN, id, x, y, 0, 0);
        LOG_ONCE("input: cursor tap at %.3f,%.3f", x, y);
    } else if (down && (x != f->x || y != f->y)) {
        SDL_Window *w = sdlb_window();
        int ww = 1280, wh = 720;
        if (w)
            SDL_GetWindowSize(w, &ww, &wh);
        if (g_cfg.touch_mouse && g_mouse_finger == id)
            touch_mouse(SDL_MOUSEMOTION, x, y, (int)((x - f->x) * ww), (int)((y - f->y) * wh));
        finger_event(SDL_FINGERMOTION, id, x, y, x - f->x, y - f->y);
        f->x = x;
        f->y = y;
    } else if (!down && f->down) {
        f->down = 0;
        if (g_cfg.touch_mouse && g_mouse_finger == id) {
            touch_mouse(SDL_MOUSEBUTTONUP, f->x, f->y, 0, 0);
            g_mouse_finger = -1;
        }
        finger_event(SDL_FINGERUP, id, f->x, f->y, 0, 0);
    }
}

static void synth_release_all(void)
{
    int i;
    for (i = 0; i < SYNTH_SLOTS; i++)
        if (g_synth[i].down)
            synth_touch(i, 0, g_synth[i].x, g_synth[i].y);
}

/* The cursor: for menus and anything the button map does not cover. */
static void update_pointer(u64 held, u64 now)
{
    const u64 chord = HidNpadButton_ZL | HidNpadButton_ZR;
    const int both = (held & chord) == chord;
    HidAnalogStickState ls;
    float cx, cy, dt;
    SDL_Window *w = sdlb_window();
    int ww = 1280, wh = 720;

    if (g_cfg.pointer && both && !g_chord_held) {
        nxp_set_visible(!nxp_visible());
        synth_release_all();               /* nothing stays pressed across the switch */
        keys_release_all();
    }
    g_chord_held = both;
    if (!nxp_visible())
        return;

    if (w)
        SDL_GetWindowSize(w, &ww, &wh);
    nxp_set_screen(ww, wh);

    dt = g_pointer_at ? (float)(now - g_pointer_at) / 1000.0f : 0.0f;
    g_pointer_at = now;
    if (dt > 0.1f)
        dt = 0.1f;                         /* after a stall, do not teleport */

    ls = padGetStickPos(&g_pad, 0);
    {
        Sint16 sx, sy;
        stick(ls.x, ls.y, &sx, &sy);       /* dead zone and rescale, y already flipped */
        if (sx || sy)
            nxp_move(sx / 32767.0f * g_cfg.pointer_speed * dt,
                     sy / 32767.0f * g_cfg.pointer_speed * dt);
    }

    nxp_pos(&cx, &cy);
    synth_touch(PTR_SLOT, (held & HidNpadButton_A) != 0, cx / (float)ww, cy / (float)wh);
}

/* Fold every input onto the control it is mapped to, then hold down the key
 * of every control that any of its inputs is holding. */
static void update_keys(u64 held, u64 now)
{
    static const u64 BTN[] = {
        HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
        HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL, HidNpadButton_ZR,
        HidNpadButton_Plus, HidNpadButton_Minus,
        HidNpadButton_Left, HidNpadButton_Right, HidNpadButton_Up, HidNpadButton_Down,
        0, 0, 0, 0,                        /* the stick, handled below */
    };
    _Static_assert(sizeof BTN / sizeof BTN[0] == IN_COUNT,
                   "BTN is out of step with the IN_* enum");
    HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
    int pressed[CTL_COUNT];
    Sint16 sx, sy;
    int i, ctl;

    memset(pressed, 0, sizeof(pressed));
    stick(ls.x, ls.y, &sx, &sy);
    for (i = 0; i < IN_COUNT; i++) {
        int down;
        ctl = g_cfg.map[i];
        if (ctl <= CTL_NONE || ctl >= CTL_COUNT)
            continue;
        switch (i) {
        case IN_SLEFT:  down = sx < -STICK_PRESS; break;
        case IN_SRIGHT: down = sx > STICK_PRESS;  break;
        case IN_SUP:    down = sy < -STICK_PRESS; break;
        case IN_SDOWN:  down = sy > STICK_PRESS;  break;
        default:        down = (held & BTN[i]) != 0; break;
        }
        if (down)
            pressed[ctl] = 1;
    }
    for (ctl = CTL_NONE + 1; ctl < CTL_COUNT; ctl++)
        set_key(ctl, pressed[ctl], now);
}

/* -------------------------------------------------------------- touch -- */

static void touch_mouse(Uint32 type, float nx, float ny, int dx, int dy)
{
    SDL_Window *w = sdlb_window();
    int ww = 1280, wh = 720;
    SDL_Event e;
    if (!w)
        return;
    SDL_GetWindowSize(w, &ww, &wh);
    memset(&e, 0, sizeof(e));
    e.type = type;
    if (type == SDL_MOUSEMOTION) {
        e.motion.windowID = SDL_GetWindowID(w);
        e.motion.which = SDL_TOUCH_MOUSEID;
        e.motion.state = SDL_BUTTON_LMASK;
        e.motion.x = (Sint32)(nx * ww);
        e.motion.y = (Sint32)(ny * wh);
        e.motion.xrel = dx;
        e.motion.yrel = dy;
    } else {
        e.button.windowID = SDL_GetWindowID(w);
        e.button.which = SDL_TOUCH_MOUSEID;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = (Sint32)(nx * ww);
        e.button.y = (Sint32)(ny * wh);
    }
    push(&e);
}

static void finger_event(Uint32 type, s32 id, float x, float y, float dx, float dy)
{
    SDL_Window *w = sdlb_window();
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.tfinger.touchId = TOUCH_DEVICE;
    e.tfinger.fingerId = id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.dx = dx;
    e.tfinger.dy = dy;
    e.tfinger.pressure = type == SDL_FINGERUP ? 0.0f : 1.0f;
    e.tfinger.windowID = w ? SDL_GetWindowID(w) : 0;
    push(&e);
}

static void update_touch(void)
{
    HidTouchScreenState st;
    Finger now[MAX_FINGERS];
    int n = 0, i, j;
    const int mouse = g_cfg.touch_mouse;

    if (hidGetTouchScreenStates(&st, 1)) {
        for (i = 0; i < st.count && n < MAX_FINGERS; i++) {
            now[n].id = (s32)st.touches[i].finger_id;
            now[n].x = (float)st.touches[i].x / 1280.0f;
            now[n].y = (float)st.touches[i].y / 720.0f;
            if (now[n].x > 1.0f) now[n].x = 1.0f;
            if (now[n].y > 1.0f) now[n].y = 1.0f;
            n++;
        }
    }

    /* Order matches SDL's own touch-to-mouse synthesis on Android:
     * mouse event first, then the finger event. */

    for (i = 0; i < g_nfingers; i++) {            /* lifted */
        int still = 0;
        for (j = 0; j < n; j++)
            if (now[j].id == g_fingers[i].id)
                still = 1;
        if (!still) {
            if (mouse && g_mouse_finger == g_fingers[i].id) {
                touch_mouse(SDL_MOUSEBUTTONUP, g_fingers[i].x, g_fingers[i].y, 0, 0);
                g_mouse_finger = -1;
            }
            finger_event(SDL_FINGERUP, g_fingers[i].id, g_fingers[i].x, g_fingers[i].y, 0, 0);
        }
    }

    for (j = 0; j < n; j++) {
        const Finger *old = NULL;
        for (i = 0; i < g_nfingers; i++)
            if (g_fingers[i].id == now[j].id)
                old = &g_fingers[i];
        if (!old) {                                /* new */
            if (mouse && g_mouse_finger < 0) {
                g_mouse_finger = now[j].id;
                touch_mouse(SDL_MOUSEMOTION, now[j].x, now[j].y, 0, 0);
                touch_mouse(SDL_MOUSEBUTTONDOWN, now[j].x, now[j].y, 0, 0);
            }
            finger_event(SDL_FINGERDOWN, now[j].id, now[j].x, now[j].y, 0, 0);
        } else if (old->x != now[j].x || old->y != now[j].y) {   /* moved */
            if (mouse && g_mouse_finger == now[j].id) {
                SDL_Window *w = sdlb_window();
                int ww = 1280, wh = 720;
                if (w)
                    SDL_GetWindowSize(w, &ww, &wh);
                touch_mouse(SDL_MOUSEMOTION, now[j].x, now[j].y,
                            (int)((now[j].x - old->x) * ww), (int)((now[j].y - old->y) * wh));
            }
            finger_event(SDL_FINGERMOTION, now[j].id, now[j].x, now[j].y,
                         now[j].x - old->x, now[j].y - old->y);
        }
    }

    memcpy(g_fingers, now, sizeof(Finger) * (size_t)n);
    g_nfingers = n;
}

/* --------------------------------------------------------------- pump -- */

/* Release everything the game currently sees as held. Used when input stops
 * being delivered, so nothing stays stuck down. */
static void release_all(void)
{
    int i;
    for (i = 0; i < XB_COUNT; i++)
        set_button(i, 0);
    for (i = 0; i < AX_COUNT; i++)
        set_axis(i, 0);
    set_hat(SDL_HAT_CENTERED);
    for (i = 0; i < g_nfingers; i++) {
        if (g_cfg.touch_mouse && g_mouse_finger == g_fingers[i].id) {
            touch_mouse(SDL_MOUSEBUTTONUP, g_fingers[i].x, g_fingers[i].y, 0, 0);
            g_mouse_finger = -1;
        }
        finger_event(SDL_FINGERUP, g_fingers[i].id, g_fingers[i].x, g_fingers[i].y, 0, 0);
    }
    g_nfingers = 0;
    synth_release_all();
    keys_release_all();
}

void input_pump(void)
{
    u64 held, now;
    if (!g_ready)
        return;
    /* Called from every SDL_PollEvent, and the game drains the queue several
     * times a frame. Sampling the pad and the touch screen once per
     * millisecond is plenty and keeps the event loop short. */
    now = now_ms();
    if (now == g_last_pump)
        return;
    g_last_pump = now;
    padUpdate(&g_pad);
    held = padGetButtons(&g_pad);

    /* While the software keyboard is up it owns the pad and the touch screen;
     * switch-sdl2 suppresses its own polling for the same reason. The quit
     * combo keeps working. */
    if (SDL_IsTextInputActive()) {
        if (!g_keyboard_active) {
            g_keyboard_active = 1;
            release_all();
            LOGI("input: software keyboard open, game input paused");
        }
        update_combo(held);
        return;
    }
    if (g_keyboard_active) {
        g_keyboard_active = 0;
        LOGI("input: software keyboard closed");
    }

    announce_joystick();
    reconnect_pad();
    if (g_announced)
        update_pad(held);
    update_pointer(held, now);
    if (g_cfg.keys && !nxp_visible())
        update_keys(held, now);
    else if (!g_cfg.keys)
        keys_release_all();
    if (g_cfg.touch)
        update_touch();
    update_combo(held);
    update_back_key(held);
}

/* ------------------------------------------------------------ queries -- */

int input_is_virtual(SDL_Joystick *joy)
{
    return g_announced && joy && SDL_JoystickInstanceID(joy) == joy_id();
}

int    input_num_buttons(void) { return XB_COUNT; }
int    input_num_axes(void)    { return AX_COUNT; }
int    input_num_hats(void)    { return 1; }
Uint8  input_button(int b)     { return (b >= 0 && b < XB_COUNT) ? g_btn[b] : 0; }
Sint16 input_axis(int a)       { return (a >= 0 && a < AX_COUNT) ? g_axis[a] : 0; }
Uint8  input_hat(int h)        { return h == 0 ? g_hat : 0; }

void input_vibrate(int ms)
{
    SDL_Joystick *j;
    if (!g_cfg.rumble || ms <= 0)
        return;
    j = SDL_JoystickFromInstanceID(joy_id());
    if (!j) {
        if (!g_rumble_joy && SDL_WasInit(SDL_INIT_JOYSTICK))
            g_rumble_joy = SDL_JoystickOpen(0);
        j = g_rumble_joy;
    }
    if (j)
        SDL_JoystickRumble(j, 0xA000, 0xA000, (Uint32)(ms > 2000 ? 2000 : ms));
}
