/* config.h -- settings.
 *
 * Everything about this port is fixed at build time except one thing: which
 * Switch button presses which of the game's controls. That is the only thing
 * config.txt carries, and it is written with the defaults on first run so the
 * choices are discoverable.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_CONFIG_H
#define HS_CONFIG_H

enum { HS_RES_AUTO = 0, HS_RES_720 = 720, HS_RES_1080 = 1080 };
enum { HS_LAYOUT_LABEL = 0, HS_LAYOUT_POSITION = 1 };
enum { HS_DPAD_HAT = 0, HS_DPAD_STICK = 1, HS_DPAD_BOTH = 2 };

/* The game's controls, exactly as its Stencyl project names them. Each one is
 * mapped to a key in the game's own config (read out of libApplicationMain.so,
 * see PORTING.md); the key each control sends is fixed in input.c. */
enum {
    CTL_NONE = 0,
    CTL_LEFT, CTL_RIGHT, CTL_UP, CTL_DOWN,
    CTL_JUMP, CTL_ACTION1, CTL_ENTER, CTL_ESCAPE,
    CTL_RESTART, CTL_ACTION2,
    CTL_COUNT
};

/* What a player can press. */
enum {
    IN_A, IN_B, IN_X, IN_Y, IN_L, IN_R, IN_ZL, IN_ZR, IN_PLUS, IN_MINUS,
    IN_DLEFT, IN_DRIGHT, IN_DUP, IN_DDOWN,
    IN_SLEFT, IN_SRIGHT, IN_SUP, IN_SDOWN, IN_COUNT
};

typedef struct {
    /* the one editable setting */
    unsigned char map[IN_COUNT];   /* CTL_* for each input */

    /* fixed at build time */
    int log_level;
    int resolution;
    int button_layout;
    int dpad;
    int stick_deadzone;
    int touch;
    int touch_mouse;
    int rumble;
    int exit_combo;
    int minus_back_key;
    int dpi;
    int game_stack_mb;
    int gamepad;
    int vsync;
    int frame_stats;
    int gc_working_mb;
    int gc_free_mb;
    int game_core;
    int pad_reconnects;
    int keys;                      /* send keys for the mapped controls */
    int key_repeat_ms;             /* 0 = no auto-repeat (see input.c) */
    int pointer;
    int pointer_speed;
} HsConfig;

extern HsConfig g_cfg;

void config_defaults(void);
/* Reads the button mapping; writes the file with the defaults if absent. */
int  config_load(const char *path);

const char *config_input_name(int input);
const char *config_control_name(int control);

#endif
