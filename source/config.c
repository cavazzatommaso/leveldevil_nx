/* config.c -- see config.h. MIT licensed, see LICENSE. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "log.h"

HsConfig g_cfg;

static const char *const INPUT_NAMES[] = {
    "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus",
    "dpad_left", "dpad_right", "dpad_up", "dpad_down",
    "stick_left", "stick_right", "stick_up", "stick_down",
};

static const char *const CONTROL_NAMES[] = {
    "none", "left", "right", "up", "down",
    "jump", "action1", "enter", "escape",
    "restart", "action2",
};

_Static_assert(sizeof INPUT_NAMES / sizeof INPUT_NAMES[0] == IN_COUNT,
               "INPUT_NAMES is out of step with the IN_* enum");
_Static_assert(sizeof CONTROL_NAMES / sizeof CONTROL_NAMES[0] == CTL_COUNT,
               "CONTROL_NAMES is out of step with the CTL_* enum");

const char *config_input_name(int i)   { return (i >= 0 && i < IN_COUNT) ? INPUT_NAMES[i] : "?"; }
const char *config_control_name(int c) { return (c >= 0 && c < CTL_COUNT) ? CONTROL_NAMES[c] : "?"; }

void config_defaults(void)
{
    int i;
    for (i = 0; i < IN_COUNT; i++)
        g_cfg.map[i] = CTL_NONE;

    /* Level Devil's key map (config/game-config.json in the binary): arrows,
     * Up/W/Space jump, Action1 = Z, Action2 = X, R, Enter, Esc.
     * ponytail: action1/action2 are guesses until tested on hardware. */
    g_cfg.map[IN_A]      = CTL_UP;       /* the game jumps on Up, not Space */
    g_cfg.map[IN_B]      = CTL_JUMP;
    g_cfg.map[IN_X]      = CTL_ACTION1;
    g_cfg.map[IN_Y]      = CTL_ACTION2;
    g_cfg.map[IN_R]      = CTL_RESTART;
    g_cfg.map[IN_PLUS]   = CTL_ENTER;
    g_cfg.map[IN_MINUS]  = CTL_ESCAPE;
    g_cfg.map[IN_DLEFT]  = CTL_LEFT;
    g_cfg.map[IN_DRIGHT] = CTL_RIGHT;
    g_cfg.map[IN_DUP]    = CTL_UP;
    g_cfg.map[IN_DDOWN]  = CTL_DOWN;
    g_cfg.map[IN_SLEFT]  = CTL_LEFT;
    g_cfg.map[IN_SRIGHT] = CTL_RIGHT;
    g_cfg.map[IN_SUP]    = CTL_UP;
    g_cfg.map[IN_SDOWN]  = CTL_DOWN;

    /* Fixed. These were settings once; they are decided now. */
    g_cfg.log_level      = HS_LOG_OFF;
    g_cfg.resolution     = HS_RES_AUTO;    /* 720p handheld, 1080p docked */
    g_cfg.button_layout  = HS_LAYOUT_LABEL;
    g_cfg.dpad           = HS_DPAD_HAT;
    g_cfg.stick_deadzone = 7000;
    g_cfg.touch          = 1;
    g_cfg.touch_mouse    = 1;
    g_cfg.rumble         = 1;
    g_cfg.exit_combo     = 0;              /* no quit chord: use HOME */
    g_cfg.minus_back_key = 0;
    g_cfg.dpi            = 160;
    g_cfg.game_stack_mb  = 16;
    g_cfg.gamepad        = 1;
    g_cfg.vsync          = 1;
    g_cfg.frame_stats    = 0;
    g_cfg.gc_working_mb  = 64;
    g_cfg.gc_free_mb     = 24;
    g_cfg.game_core      = -1;
    g_cfg.pad_reconnects = 4;
    g_cfg.keys           = 1;
    g_cfg.key_repeat_ms  = 0;
    g_cfg.pointer        = 1;
    g_cfg.pointer_speed  = 1100;
}

static const char DEFAULT_TEXT[] =
"# Level Devil -- Nintendo Switch port\n"
"#\n"
"# Which Switch input presses which of the game's controls.\n"
"# Everything else about the port is fixed.\n"
"#\n"
"# Controls you can assign:\n"
"#   jump        jump (Space)\n"
"#   action1     the game's Action1 key (Z)\n"
"#   action2     the game's Action2 key (X)\n"
"#   restart     R\n"
"#   left        move left\n"
"#   right       move right\n"
"#   up          up (menus, level select)\n"
"#   down        down (menus, level select)\n"
"#   enter       enter / select\n"
"#   escape      back / pause\n"
"#   none        nothing\n"
"#\n"
"# Several inputs may share one control: the D-pad and the left stick both\n"
"# work the arrows, A and B both jump. A shared\n"
"# control stays down until every input holding it lets go.\n"
"#\n"
"# ZL and ZR together always raise the on-screen cursor, whatever they are\n"
"# assigned to here. Delete this file to restore the defaults.\n"
"\n"
"a = up\n"
"b = jump\n"
"x = action1\n"
"y = action2\n"
"l = none\n"
"r = restart\n"
"zl = none\n"
"zr = none\n"
"plus = enter\n"
"minus = escape\n"
"\n"
"dpad_left = left\n"
"dpad_right = right\n"
"dpad_up = up\n"
"dpad_down = down\n"
"\n"
"stick_left = left\n"
"stick_right = right\n"
"stick_up = up\n"
"stick_down = down\n";

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static void apply(const char *k, const char *v)
{
    int i, c;

    for (i = 0; i < IN_COUNT; i++) {
        if (strcasecmp(k, INPUT_NAMES[i]) != 0)
            continue;
        for (c = 0; c < CTL_COUNT; c++) {
            if (!strcasecmp(v, CONTROL_NAMES[c])) {
                g_cfg.map[i] = (unsigned char)c;
                return;
            }
        }
        LOGI("config: '%s' is not a control name, leaving %s as %s", v,
             INPUT_NAMES[i], CONTROL_NAMES[g_cfg.map[i]]);
        return;
    }
    /* Undocumented, for working out why something misbehaves: log_level = 1
     * (or 2) turns leveldevil.log back on. */
    if (!strcasecmp(k, "log_level")) {
        g_cfg.log_level = atoi(v);
        return;
    }
    LOGI("config: unknown key '%s'", k);
}

int config_load(const char *path)
{
    char line[256];
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fp = fopen(path, "w");
        if (fp) {
            fputs(DEFAULT_TEXT, fp);
            fclose(fp);
        }
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line), *eq;
        if (!*s || *s == '#' || *s == ';')
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        apply(trim(s), trim(eq + 1));
    }
    fclose(fp);
    return 1;
}
