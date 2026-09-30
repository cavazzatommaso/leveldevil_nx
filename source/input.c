/* input.c -- the pad and the touch screen, as Android input events.
 *
 * Defold's NativeActivity glue reads AInputQueue: key events go through its
 * Android keycode table to Defold's keys (DPAD_LEFT is key-left, SPACE is
 * key-space...), touch goes to touch/mouse. So the pad presses keys --
 * config.txt says which input presses which -- and the touch screen is passed
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

/* android/keycodes.h, in PB_C_* order */
static const int KEYCODE[PB_C_COUNT] = {
    21,     /* DPAD_LEFT  */
    22,     /* DPAD_RIGHT */
    19,     /* DPAD_UP    */
    20,     /* DPAD_DOWN  */
    62,     /* SPACE      */
    66,     /* ENTER      */
    111,    /* ESCAPE     */
};

#define MAX_FINGERS   8
#define CURSOR_ID     9
#define STICK_MAX     32767.0f
#define STICK_ON      0.4f

static PadState g_pad;
static HidTouchScreenState g_touch;
static int g_key_down[PB_C_COUNT];

/* Every pointer that is down: real fingers by libnx id, plus the cursor. */
static struct { int active; u32 nx_id; float x, y; } g_ptr[MAX_FINGERS + 2];

void input_init(void)
{
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
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

static unsigned axis_state(void)
{
    HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
    float lx = stick_norm(l.x), ly = stick_norm(l.y), rx = stick_norm(r.x), ry = stick_norm(r.y);
    unsigned a = 0;
    if (lx < -STICK_ON) a |= PB_AX_L_LEFT;
    if (lx >  STICK_ON) a |= PB_AX_L_RIGHT;
    if (ly >  STICK_ON) a |= PB_AX_L_UP;        /* libnx y is up */
    if (ly < -STICK_ON) a |= PB_AX_L_DOWN;
    if (rx < -STICK_ON) a |= PB_AX_R_LEFT;
    if (rx >  STICK_ON) a |= PB_AX_R_RIGHT;
    if (ry >  STICK_ON) a |= PB_AX_R_UP;
    if (ry < -STICK_ON) a |= PB_AX_R_DOWN;
    return a;
}

static void set_keys(const int want[PB_C_COUNT])
{
    int c;
    for (c = 0; c < PB_C_COUNT; c++) {
        if (want[c] == g_key_down[c])
            continue;
        g_key_down[c] = want[c];
        lp_push_key(KEYCODE[c], want[c]);
    }
}

static void pump_keys(u64 held)
{
    unsigned bits = 0, axes = axis_state();
    int want[PB_C_COUNT], c;
    size_t i;

    for (i = 0; i < sizeof(BTN_BITS) / sizeof(BTN_BITS[0]); i++)
        if (held & BTN_BITS[i].btn)
            bits |= BTN_BITS[i].bit;
    for (c = 0; c < PB_C_COUNT; c++)
        want[c] = (g_cfg.map_btn[c] & bits) || (g_cfg.map_axis[c] & axes);
    if (want[PB_C_LEFT] && want[PB_C_RIGHT])
        want[PB_C_LEFT] = want[PB_C_RIGHT] = 0;
    set_keys(want);
}

/* -------------------------------------------------------------- cursor --- */

static int g_chord_held, g_cursor_tapping;
static u64 g_cursor_tick;

/* 1 while the cursor is up: the caller leaves the keys alone. */
static int pump_cursor(u64 held)
{
    const u64 chord = HidNpadButton_ZL | HidNpadButton_ZR;
    int both = (held & chord) == chord;
    float cx, cy, dt;
    u64 now;

    if (!g_cfg.cursor)
        return 0;
    if (both && !g_chord_held) {
        static const int none[PB_C_COUNT];
        nxp_set_visible(!nxp_visible());
        set_keys(none);
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
    {
        HidAnalogStickState ls = padGetStickPos(&g_pad, 0);
        float sx = stick_norm(ls.x), sy = stick_norm(ls.y);
        if (sx != 0.0f || sy != 0.0f)
            nxp_move(sx * (float)g_cfg.cursor_speed * dt, -sy * (float)g_cfg.cursor_speed * dt);
    }
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

/* ---------------------------------------------------------------- pump --- */

void input_pump(void)
{
    u64 held;

    padUpdate(&g_pad);
    held = padGetButtons(&g_pad);
    if (g_cfg.exit_combo && (held & HidNpadButton_Plus) && (held & HidNpadButton_Minus)) {
        LOGI("input: + and - held; quitting");
        pb_request_exit(0);
        return;
    }
    pump_touch();
    if (!pump_cursor(held))
        pump_keys(held);
}
