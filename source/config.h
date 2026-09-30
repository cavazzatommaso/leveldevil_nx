/* config.h -- settings. config.txt maps Switch inputs to the game's keys and
 * is written with the defaults on first run. MIT licensed, see LICENSE. */
#ifndef PB_CONFIG_H
#define PB_CONFIG_H

enum { PB_RES_AUTO = 0, PB_RES_720 = 720, PB_RES_1080 = 1080 };

/* The game's keys a Switch input can press (see input.c for the keycodes). */
enum { PB_C_LEFT, PB_C_RIGHT, PB_C_UP, PB_C_DOWN, PB_C_SPACE, PB_C_ENTER,
       PB_C_ESCAPE, PB_C_COUNT };

/* Which Switch input presses a control. These are the port's own bits, not
 * libnx's, so config.c stays free of <switch.h> and testable on a PC;
 * input.c translates them. */
#define PB_BTN_A       (1u <<  0)
#define PB_BTN_B       (1u <<  1)
#define PB_BTN_X       (1u <<  2)
#define PB_BTN_Y       (1u <<  3)
#define PB_BTN_L       (1u <<  4)
#define PB_BTN_R       (1u <<  5)
#define PB_BTN_ZL      (1u <<  6)
#define PB_BTN_ZR      (1u <<  7)
#define PB_BTN_PLUS    (1u <<  8)
#define PB_BTN_MINUS   (1u <<  9)
#define PB_BTN_LSTICK  (1u << 10)
#define PB_BTN_RSTICK  (1u << 11)
#define PB_BTN_UP      (1u << 12)
#define PB_BTN_DOWN    (1u << 13)
#define PB_BTN_LEFT    (1u << 14)
#define PB_BTN_RIGHT   (1u << 15)

/* Stick directions, past the dead zone. */
#define PB_AX_L_LEFT   (1u << 0)
#define PB_AX_L_RIGHT  (1u << 1)
#define PB_AX_L_UP     (1u << 2)
#define PB_AX_L_DOWN   (1u << 3)
#define PB_AX_R_LEFT   (1u << 4)
#define PB_AX_R_RIGHT  (1u << 5)
#define PB_AX_R_UP     (1u << 6)
#define PB_AX_R_DOWN   (1u << 7)

typedef struct {
    /* the editable settings */
    /* Which inputs press each key. */
    unsigned map_btn[PB_C_COUNT];
    unsigned map_axis[PB_C_COUNT];
    int cursor;              /* ZL+ZR raises the virtual cursor         */
    int cursor_speed;        /* pixels a second at full stick           */
    int log_level;
    int resolution;          /* PB_RES_*                                    */

    /* fixed at build time */
    int stick_deadzone;
    int touch;
    int exit_combo;
    int dpi;
    int game_stack_mb;
    int vsync;
    int frame_stats;
    int game_core;
} PbConfig;

extern PbConfig g_cfg;

void config_defaults(void);
/* Reads the file; writes it with the defaults if it is not there. */
int  config_load(const char *path);

#endif
