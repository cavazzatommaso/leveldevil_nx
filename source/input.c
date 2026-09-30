/* input.c -- the pad and the touch screen, as Android input events.
 *
 * Defold's NativeActivity glue reads AInputQueue: key events go through its
 * Android keycode table to Defold's keys (DPAD_LEFT is key-left, SPACE is
 * key-space...), touch goes to touch/mouse. So the pad presses keys --
 * config.txt says which input presses which; a second controller gets player
 * 2's keys -- and the touch screen is passed
 * through as a touchscreen, with every finger in every event, the way
 * Android reports multi-touch.
 *
 * ZL + ZR raises a cursor (nx_pointer.c) for menus: the stick moves it, A taps
 * as one more finger, and while it is up the key map is silent.
 *
 * MIT licensed, see LICENSE.
 */
#include <math.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "looper.h"
#include "nativewindow.h"
#include "nx_pointer.h"

/* android/keycodes.h, in PB_C_* order.
 *
 * The game's own input binding (game.input_bindingc) is the PC keyboard
 * layout: player 1 on WASD + Space, player 2 on the arrow keys, and
 * single-player answers to either. So one controller sends the arrows, and
 * with a second one connected controller 1 becomes WASD and controller 2 the
 * arrows -- the same config.txt map for both. */
static const int KEYS_ARROWS[PB_C_COUNT] = {
    21,     /* DPAD_LEFT  */
    22,     /* DPAD_RIGHT */
    19,     /* DPAD_UP    */
    20,     /* DPAD_DOWN  */
    62,     /* SPACE      */
    66,     /* ENTER      */
    111,    /* ESCAPE     */
};
static const int KEYS_WASD[PB_C_COUNT] = {
    29,     /* A      */
    32,     /* D      */
    51,     /* W      */
    47,     /* S      */
    62,     /* SPACE  */
    66,     /* ENTER  */
    111,    /* ESCAPE */
};

#define MAX_FINGERS   8
#define CURSOR_ID     9
#define STICK_MAX     32767.0f
#define STICK_ON      0.4f

static PadState g_pad;     /* player 1: handheld or controller 1 */
static PadState g_pad2;    /* player 2 */
static HidTouchScreenState g_touch;
static int g_two_players;

typedef struct {
    PadState  *pad;
    const int *keys;
    int        down[PB_C_COUNT];
} Player;

static Player g_players[2] = {
    { &g_pad, KEYS_ARROWS, { 0 } },
    { &g_pad2, KEYS_ARROWS, { 0 } },
};

/* Every pointer that is down: real fingers by libnx id, plus the cursor. */
static struct { int active; u32 nx_id; float x, y; } g_ptr[MAX_FINGERS + 2];

void input_init(void)
{
    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    padInitialize(&g_pad2, HidNpadIdType_No2);
    /* Horizontal, so the Controllers applet shows single Joy-Cons sideways.
     * The reported data is raw either way; read_pad does the rotation. */
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    hidInitializeTouchScreen();
    LOGI("input: pad -> keys, touch %s, cursor %s", g_cfg.touch ? "on" : "off",
         g_cfg.cursor ? "on ZL+ZR" : "off");
}

/* ------------------------------------------------------------ pointers --- */

static void send_motion(int action, int index)
{
    int ids[MAX_FINGERS + 2], i, n = 0, pos = 0;
    float xs[MAX_FINGERS + 2], ys[MAX_FINGERS + 2];

    for (i = 0; i < MAX_FINGERS + 2; i++) {
        if (!g_ptr[i].active)
            continue;
        if (i == index)
            pos = n;
        ids[n] = i;
        xs[n] = g_ptr[i].x;
        ys[n] = g_ptr[i].y;
        n++;
    }
    if (action == LP_MOTION_POINTER_DOWN || action == LP_MOTION_POINTER_UP)
        action |= pos << 8;
    lp_push_motion(action, n, ids, xs, ys);
}

static int active_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_FINGERS + 2; i++)
        n += g_ptr[i].active;
    return n;
}

static void ptr_down(int slot, float x, float y)
{
    g_ptr[slot].active = 1;
    g_ptr[slot].x = x;
    g_ptr[slot].y = y;
    send_motion(active_count() == 1 ? LP_MOTION_DOWN : LP_MOTION_POINTER_DOWN, slot);
}

static void ptr_up(int slot)
{
    /* The lifting pointer is still in the event, as on Android. */
    send_motion(active_count() == 1 ? LP_MOTION_UP : LP_MOTION_POINTER_UP, slot);
    g_ptr[slot].active = 0;
}

static void pump_touch(void)
{
    int seen[MAX_FINGERS] = { 0 };
    int i, j, count, moved = 0;
    float sx = (float)pbnw_width() / 1280.0f, sy = (float)pbnw_height() / 720.0f;

    if (!g_cfg.touch)
        return;
    count = hidGetTouchScreenStates(&g_touch, 1) ? (int)g_touch.count : 0;
    if (count > MAX_FINGERS)
        count = MAX_FINGERS;

    for (i = 0; i < count; i++) {
        u32 id = g_touch.touches[i].finger_id;
        float x = (float)g_touch.touches[i].x * sx, y = (float)g_touch.touches[i].y * sy;
        int slot = -1;
        for (j = 0; j < MAX_FINGERS; j++)
            if (g_ptr[j].active && g_ptr[j].nx_id == id)
                slot = j;
        if (slot < 0) {
            for (j = 0; j < MAX_FINGERS && g_ptr[j].active; j++)
                ;
            if (j == MAX_FINGERS)
                continue;
            seen[j] = 1;
            g_ptr[j].nx_id = id;
            ptr_down(j, x, y);
            continue;
        }
        seen[slot] = 1;
        if (x != g_ptr[slot].x || y != g_ptr[slot].y) {
            g_ptr[slot].x = x;
            g_ptr[slot].y = y;
            moved = 1;
        }
    }
    if (moved)
        send_motion(LP_MOTION_MOVE, -1);
    for (j = 0; j < MAX_FINGERS; j++)
        if (g_ptr[j].active && !seen[j])
            ptr_up(j);
}

/* ---------------------------------------------------------------- keys --- */

static const struct { unsigned bit; u64 btn; } BTN_BITS[] = {
    { PB_BTN_A, HidNpadButton_A }, { PB_BTN_B, HidNpadButton_B },
    { PB_BTN_X, HidNpadButton_X }, { PB_BTN_Y, HidNpadButton_Y },
    { PB_BTN_L, HidNpadButton_L }, { PB_BTN_R, HidNpadButton_R },
    { PB_BTN_ZL, HidNpadButton_ZL }, { PB_BTN_ZR, HidNpadButton_ZR },
    { PB_BTN_PLUS, HidNpadButton_Plus }, { PB_BTN_MINUS, HidNpadButton_Minus },
    { PB_BTN_LSTICK, HidNpadButton_StickL }, { PB_BTN_RSTICK, HidNpadButton_StickR },
    { PB_BTN_UP, HidNpadButton_Up }, { PB_BTN_DOWN, HidNpadButton_Down },
    { PB_BTN_LEFT, HidNpadButton_Left }, { PB_BTN_RIGHT, HidNpadButton_Right },
};

static float stick_norm(s32 v)
{
    float f = (float)v / STICK_MAX, dz = (float)g_cfg.stick_deadzone / STICK_MAX;
    if (f > 1.0f)  f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    if (fabsf(f) < dz)
        return 0.0f;
    return (f > 0 ? (f - dz) : (f + dz)) / (1.0f - dz);
}

/* One controller's buttons and main stick, as if it were a full controller.
 * A single Joy-Con is held sideways, rail up, so its stick and face buttons
 * are rotated a quarter turn: the left one counter-clockwise, the right one
 * clockwise. Everything else is read as-is. */
typedef struct { u64 held; float x, y; } PadRead;

static PadRead read_pad(PadState *pad)
{
    u32 style = padGetStyleSet(pad);
    u64 raw = padGetButtons(pad), h = 0;
    PadRead r;

    if (style & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual) ||
        !(style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))) {
        HidAnalogStickState l = padGetStickPos(pad, 0);
        r.held = raw;
        r.x = stick_norm(l.x);
        r.y = stick_norm(l.y);
        return r;
    }
    if (style & HidNpadStyleTag_NpadJoyLeft) {
        HidAnalogStickState st = padGetStickPos(pad, 0);
        if (raw & HidNpadButton_Down)    h |= HidNpadButton_A;
        if (raw & HidNpadButton_Right)   h |= HidNpadButton_X;
        if (raw & HidNpadButton_Up)      h |= HidNpadButton_Y;
        if (raw & HidNpadButton_Left)    h |= HidNpadButton_B;
        if (raw & HidNpadButton_LeftSL)  h |= HidNpadButton_L;
        if (raw & HidNpadButton_LeftSR)  h |= HidNpadButton_R;
        h |= raw & (HidNpadButton_Minus | HidNpadButton_StickL | HidNpadButton_ZL);
        r.x = -stick_norm(st.y);
        r.y = stick_norm(st.x);
    } else {
        HidAnalogStickState st = padGetStickPos(pad, 1);
        if (raw & HidNpadButton_X)       h |= HidNpadButton_A;
        if (raw & HidNpadButton_A)       h |= HidNpadButton_B;
        if (raw & HidNpadButton_B)       h |= HidNpadButton_Y;
        if (raw & HidNpadButton_Y)       h |= HidNpadButton_X;
        if (raw & HidNpadButton_RightSL) h |= HidNpadButton_L;
        if (raw & HidNpadButton_RightSR) h |= HidNpadButton_R;
        h |= raw & (HidNpadButton_Plus | HidNpadButton_StickR | HidNpadButton_ZR);
        r.x = stick_norm(st.y);
        r.y = -stick_norm(st.x);
    }
    r.held = h;
    return r;
}

static unsigned axis_state(const PadRead *p)
{
    unsigned a = 0;
    if (p->x < -STICK_ON) a |= PB_AX_L_LEFT;
    if (p->x >  STICK_ON) a |= PB_AX_L_RIGHT;
    if (p->y >  STICK_ON) a |= PB_AX_L_UP;        /* libnx y is up */
    if (p->y < -STICK_ON) a |= PB_AX_L_DOWN;
    return a;
}

static void set_keys(Player *pl, const int want[PB_C_COUNT])
{
    int c;
    for (c = 0; c < PB_C_COUNT; c++) {
        if (want[c] == pl->down[c])
            continue;
        pl->down[c] = want[c];
        lp_push_key(pl->keys[c], want[c]);
    }
}

static void release_keys(Player *pl)
{
    static const int none[PB_C_COUNT];
    set_keys(pl, none);
}

static void pump_keys(Player *pl, const PadRead *p)
{
    u64 held = p->held;
    unsigned bits = 0, axes = axis_state(p);
    int want[PB_C_COUNT], c;
    size_t i;

    for (i = 0; i < sizeof(BTN_BITS) / sizeof(BTN_BITS[0]); i++)
        if (held & BTN_BITS[i].btn)
            bits |= BTN_BITS[i].bit;
    for (c = 0; c < PB_C_COUNT; c++)
        want[c] = (g_cfg.map_btn[c] & bits) || (g_cfg.map_axis[c] & axes);
    if (want[PB_C_LEFT] && want[PB_C_RIGHT])
        want[PB_C_LEFT] = want[PB_C_RIGHT] = 0;
    /* Space is player 1's jump in the game's bindings whoever presses it, so
     * player 2's space button jumps with player 2's key (up) instead. */
    if (g_two_players && pl == &g_players[1]) {
        want[PB_C_UP] |= want[PB_C_SPACE];
        want[PB_C_SPACE] = 0;
    }
    set_keys(pl, want);
}

/* Controller 2 connected or gone: switch layouts with every key released, so
 * nothing stays held across the change. */
static void follow_players(void)
{
    int two = padIsConnected(&g_pad2);
    if (two == g_two_players)
        return;
    release_keys(&g_players[0]);
    release_keys(&g_players[1]);
    g_two_players = two;
    g_players[0].keys = two ? KEYS_WASD : KEYS_ARROWS;
    LOGI("input: %s", two ? "two controllers: 1 = WASD (player 1), 2 = arrows (player 2)"
                          : "one controller: arrows");
    LOGI("input: controller styles 0x%x / 0x%x (0x8 left Joy-Con, 0x10 right Joy-Con)",
         (unsigned)padGetStyleSet(&g_pad), (unsigned)padGetStyleSet(&g_pad2));
}

/* -------------------------------------------------------------- cursor --- */

static int g_chord_held, g_cursor_tapping;
static u64 g_cursor_tick;

/* 1 while the cursor is up: the caller leaves the keys alone. */
static int pump_cursor(const PadRead *p)
{
    u64 held = p->held;
    const u64 chord = HidNpadButton_ZL | HidNpadButton_ZR;
    int both = (held & chord) == chord;
    float cx, cy, dt;
    u64 now;

    if (!g_cfg.cursor)
        return 0;
    if (both && !g_chord_held) {
        nxp_set_visible(!nxp_visible());
        release_keys(&g_players[0]);
        if (g_cursor_tapping) {
            ptr_up(CURSOR_ID);
            g_cursor_tapping = 0;
        }
        g_cursor_tick = 0;
        LOGI("input: cursor %s", nxp_visible() ? "up" : "away");
    }
    g_chord_held = both;
    if (!nxp_visible())
        return 0;

    nxp_set_screen(pbnw_width(), pbnw_height());
    now = armGetSystemTick();
    dt = g_cursor_tick ? (float)armTicksToNs(now - g_cursor_tick) / 1e9f : 0.0f;
    g_cursor_tick = now;
    if (dt > 0.1f)
        dt = 0.1f;
    if (p->x != 0.0f || p->y != 0.0f)
        nxp_move(p->x * (float)g_cfg.cursor_speed * dt, -p->y * (float)g_cfg.cursor_speed * dt);
    nxp_pos(&cx, &cy);
    {
        int want = (held & HidNpadButton_A) != 0;
        if (want && !g_cursor_tapping) {
            ptr_down(CURSOR_ID, cx, cy);
        } else if (want && (cx != g_ptr[CURSOR_ID].x || cy != g_ptr[CURSOR_ID].y)) {
            g_ptr[CURSOR_ID].x = cx;
            g_ptr[CURSOR_ID].y = cy;
            send_motion(LP_MOTION_MOVE, -1);
        } else if (!want && g_cursor_tapping) {
            ptr_up(CURSOR_ID);
        }
        g_cursor_tapping = want;
    }
    return 1;
}

/* ------------------------------------------------- controllers applet --- */
/* The system "Controllers" screen games show when the setup changes: pick one
 * or two players, pair Joy-Cons or use them separately. It opens by itself
 * when the Joy-Cons come off the console, and on L + R held for a second
 * (SL + SR on a sideways Joy-Con). It blocks until closed, which is fine on
 * this thread -- the game keeps running on its own. */
#define APPLET_HOLD_NS 1000000000ULL

static int g_handheld_prev = -1;
static u64 g_lr_since;

static void show_controllers(const char *why)
{
    HidLaControllerSupportArg arg;
    HidLaControllerSupportResultInfo info;
    Result rc;

    release_keys(&g_players[0]);
    release_keys(&g_players[1]);
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 1;
    arg.hdr.player_count_max = 2;
    arg.hdr.enable_take_over_connection = 1;
    arg.hdr.enable_left_justify = 1;
    arg.hdr.enable_permit_joy_dual = 1;
    memset(&info, 0, sizeof(info));
    LOGI("input: opening the Controllers applet (%s)", why);
    rc = hidLaShowControllerSupport(&info, &arg);
    if (R_FAILED(rc))
        LOGI("input: Controllers applet failed 0x%x", rc);
    else
        LOGI("input: Controllers applet closed, %d player(s)", info.player_count);
}

static void pump_applet(const PadRead *p1, const PadRead *p2)
{
    const u64 lr = HidNpadButton_L | HidNpadButton_R;
    int handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0;
    int held = (p1->held & lr) == lr || (p2 && (p2->held & lr) == lr);

    if (g_handheld_prev == 1 && !handheld) {
        g_handheld_prev = handheld;
        show_controllers("Joy-Cons detached");
        return;
    }
    g_handheld_prev = handheld;

    if (!held) {
        g_lr_since = 0;
        return;
    }
    if (!g_lr_since) {
        g_lr_since = armGetSystemTick();
    } else if (armTicksToNs(armGetSystemTick() - g_lr_since) >= APPLET_HOLD_NS) {
        g_lr_since = 0;
        show_controllers("L + R held");
    }
}

/* ---------------------------------------------------------------- pump --- */

void input_pump(void)
{
    PadRead p1, p2;
    u64 held;

    padUpdate(&g_pad);
    p1 = read_pad(&g_pad);
    held = p1.held;
    if (g_cfg.exit_combo && (held & HidNpadButton_Plus) && (held & HidNpadButton_Minus)) {
        LOGI("input: + and - held; quitting");
        pb_request_exit(0);
        return;
    }
    padUpdate(&g_pad2);
    p2 = read_pad(&g_pad2);
    pump_applet(&p1, padIsConnected(&g_pad2) ? &p2 : NULL);
    follow_players();
    pump_touch();
    if (!pump_cursor(&p1))
        pump_keys(&g_players[0], &p1);
    if (g_two_players)
        pump_keys(&g_players[1], &p2);
}
