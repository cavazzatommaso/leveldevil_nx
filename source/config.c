/* config.c -- read config.txt, or write it with the defaults. Unknown keys
 * are ignored and logged. MIT licensed, see LICENSE. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"

PbConfig g_cfg;

void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    /* Level Devil jumps on Up, like the Stencyl build: A, the D-pad and the
     * stick all do it. */
    g_cfg.map_btn[PB_C_LEFT]    = PB_BTN_LEFT;
    g_cfg.map_axis[PB_C_LEFT]   = PB_AX_L_LEFT;
    g_cfg.map_btn[PB_C_RIGHT]   = PB_BTN_RIGHT;
    g_cfg.map_axis[PB_C_RIGHT]  = PB_AX_L_RIGHT;
    g_cfg.map_btn[PB_C_UP]      = PB_BTN_UP | PB_BTN_A;
    g_cfg.map_axis[PB_C_UP]     = PB_AX_L_UP;
    g_cfg.map_btn[PB_C_DOWN]    = PB_BTN_DOWN;
    g_cfg.map_axis[PB_C_DOWN]   = PB_AX_L_DOWN;
    g_cfg.map_btn[PB_C_SPACE]   = PB_BTN_B;
    g_cfg.map_btn[PB_C_ENTER]   = PB_BTN_PLUS;
    g_cfg.map_btn[PB_C_ESCAPE]  = PB_BTN_MINUS;

    g_cfg.cursor = 1;
    g_cfg.cursor_speed = 900;
    g_cfg.log_level = PB_LOG_OFF;
    g_cfg.resolution = PB_RES_AUTO;

    g_cfg.stick_deadzone = 5000;
    g_cfg.touch = 1;
    g_cfg.exit_combo = 0;              /* no quit chord: use HOME */
    g_cfg.dpi = 160;
    g_cfg.game_stack_mb = 16;
    g_cfg.vsync = 1;
    g_cfg.frame_stats = 0;
    g_cfg.game_core = -1;
}

static const char DEFAULT_FILE[] =
"# Level Devil for Nintendo Switch\n"
"#\n"
"# Which Switch input presses which of the game's keys. List as many inputs\n"
"# as you like per key, separated by commas; any of them works.\n"
"#\n"
"#   a b x y  l r  zl zr  plus minus  lstick rstick\n"
"#   dpad_up dpad_down dpad_left dpad_right\n"
"#   stick_left stick_right stick_up stick_down\n"
"#   rstick_left rstick_right rstick_up rstick_down\n"
"#   none\n"
"#\n"
"# The game jumps on Up.\n"
"\n"
"map_left   = dpad_left, stick_left\n"
"map_right  = dpad_right, stick_right\n"
"map_up     = a, dpad_up, stick_up\n"
"map_down   = dpad_down, stick_down\n"
"map_space  = b\n"
"map_enter  = plus\n"
"map_escape = minus\n"
"\n"
"# ZL + ZR raises a cursor for menus: the left stick moves it, A taps.\n"
"# Press ZL + ZR again to put it away. The touch screen always works.\n"
"\n"
"cursor = 1\n"
"cursor_speed = 900\n"
"\n"
"# auto follows the dock (720p handheld, 1080p docked); 720 or 1080 pins it.\n"
"\n"
"resolution = auto\n"
"\n"
"# 0 off, 1 normal, 2 verbose. The log is leveldevil.log next to the .nro.\n"
"\n"
"log_level = 0\n";

static void trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int apply(const char *key, const char *val)
{
    if (!strcmp(key, "resolution")) {
        if (!strcmp(val, "auto"))       g_cfg.resolution = PB_RES_AUTO;
        else if (!strcmp(val, "720"))   g_cfg.resolution = PB_RES_720;
        else if (!strcmp(val, "1080"))  g_cfg.resolution = PB_RES_1080;
        else return 0;
        return 1;
    }
    {
        static const struct { const char *name; unsigned btn, axis; } TOKENS[] = {
            { "a", PB_BTN_A, 0 }, { "b", PB_BTN_B, 0 },
            { "x", PB_BTN_X, 0 }, { "y", PB_BTN_Y, 0 },
            { "l", PB_BTN_L, 0 }, { "r", PB_BTN_R, 0 },
            { "zl", PB_BTN_ZL, 0 }, { "zr", PB_BTN_ZR, 0 },
            { "plus", PB_BTN_PLUS, 0 }, { "minus", PB_BTN_MINUS, 0 },
            { "lstick", PB_BTN_LSTICK, 0 }, { "rstick", PB_BTN_RSTICK, 0 },
            { "dpad_up", PB_BTN_UP, 0 }, { "dpad_down", PB_BTN_DOWN, 0 },
            { "dpad_left", PB_BTN_LEFT, 0 }, { "dpad_right", PB_BTN_RIGHT, 0 },
            { "stick_left", 0, PB_AX_L_LEFT }, { "stick_right", 0, PB_AX_L_RIGHT },
            { "stick_up", 0, PB_AX_L_UP }, { "stick_down", 0, PB_AX_L_DOWN },
            { "rstick_left", 0, PB_AX_R_LEFT }, { "rstick_right", 0, PB_AX_R_RIGHT },
            { "rstick_up", 0, PB_AX_R_UP }, { "rstick_down", 0, PB_AX_R_DOWN },
            { "none", 0, 0 },
        };
        static const struct { const char *key; int ctl; } MAPS[] = {
            { "map_left", PB_C_LEFT }, { "map_right", PB_C_RIGHT },
            { "map_up", PB_C_UP }, { "map_down", PB_C_DOWN },
            { "map_space", PB_C_SPACE }, { "map_enter", PB_C_ENTER },
            { "map_escape", PB_C_ESCAPE },
        };
        size_t m;
        for (m = 0; m < sizeof(MAPS) / sizeof(MAPS[0]); m++) {
            char list[256], *save = NULL, *tok;
            unsigned btn = 0, axis = 0;
            int bad = 0;

            if (strcmp(key, MAPS[m].key))
                continue;
            snprintf(list, sizeof(list), "%s", val);
            for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                size_t t;
                int found = 0;
                trim(tok);
                if (!*tok)
                    continue;
                for (t = 0; t < sizeof(TOKENS) / sizeof(TOKENS[0]); t++) {
                    if (strcmp(tok, TOKENS[t].name))
                        continue;
                    btn |= TOKENS[t].btn;
                    axis |= TOKENS[t].axis;
                    found = 1;
                    break;
                }
                if (!found) {
                    LOGI("config: %s has no input called \"%s\"", MAPS[m].key, tok);
                    bad = 1;
                }
            }
            if (bad)
                return 0;           /* leave the default rather than half-apply */
            g_cfg.map_btn[MAPS[m].ctl] = btn;
            g_cfg.map_axis[MAPS[m].ctl] = axis;
            return 1;
        }
    }
    if (!strcmp(key, "cursor"))            { g_cfg.cursor = atoi(val) != 0; return 1; }
    if (!strcmp(key, "cursor_speed"))      { g_cfg.cursor_speed = clamp(atoi(val), 100, 4000); return 1; }
    if (!strcmp(key, "log_level"))         { g_cfg.log_level = clamp(atoi(val), 0, 2); return 1; }
    if (!strcmp(key, "touch"))             { g_cfg.touch = atoi(val) != 0; return 1; }
    if (!strcmp(key, "exit_combo"))        { g_cfg.exit_combo = atoi(val) != 0; return 1; }
    if (!strcmp(key, "frame_stats"))       { g_cfg.frame_stats = atoi(val) != 0; return 1; }
    if (!strcmp(key, "stick_deadzone"))    { g_cfg.stick_deadzone = clamp(atoi(val), 0, 30000); return 1; }
    if (!strcmp(key, "game_stack_mb"))     { g_cfg.game_stack_mb = clamp(atoi(val), 1, 64); return 1; }
    if (!strcmp(key, "game_core"))         { g_cfg.game_core = clamp(atoi(val), -1, 3); return 1; }
    return 0;
}

static void write_defaults(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        LOGI("config: could not write %s; the defaults still apply", path);
        return;
    }
    fwrite(DEFAULT_FILE, 1, sizeof(DEFAULT_FILE) - 1, fp);
    fclose(fp);
    LOGI("config: wrote %s with the defaults", path);
}

int config_load(const char *path)
{
    char line[512];
    FILE *fp;
    int n = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        write_defaults(path);
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *eq, *key, *val;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        trim(key);
        trim(val);
        if (!*key)
            continue;
        if (apply(key, val))
            n++;
        else
            LOGI("config: ignoring \"%s = %s\"", key, val);
    }
    fclose(fp);
    LOGI("config: %d setting(s) read from %s", n, path);
    return n;
}
