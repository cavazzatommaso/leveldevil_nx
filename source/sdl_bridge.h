/* sdl_bridge.h -- liblime's SDL 2.0.12 runs on switch-sdl2.
 *
 * liblime.so contains a complete Android build of SDL 2.0.12 with the Dynamic
 * API enabled. On first use it reads $SDL_DYNAMIC_API, dlopen()s that library
 * and calls SDL_DYNAPI_entry to fill a 704-slot function table; every SDL_*
 * function inside liblime jumps through that table.
 *
 * hs_SDL_DYNAPI_entry fills the table with switch-sdl2 (SDL 2.28.x, which is
 * ABI compatible with 2.0.12 for all 694 non-Android entries), ten local
 * implementations for the Android-only entries, and overrides for the calls
 * whose Android behaviour the game depends on:
 *
 *   events      poll libnx input first; drop SDL's own joystick, controller,
 *               finger and touch-mouse events (input.c supplies the game's)
 *   window      created at the real output size, resizable so docking
 *               switches between 720p and 1080p; display modes report the
 *               current output size, not a fixed 1920x1080
 *   paths       base path = assets/, pref path = save/, Android paths mapped
 *   dialogs     message boxes go to the log
 *
 * Before trusting the slot order it decodes liblime's exported stubs and
 * checks them against sdl_procs.h; on any disagreement it fills the table by
 * name instead, which is slower to start but still correct.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_SDL_BRIDGE_H
#define HS_SDL_BRIDGE_H

#include <stdint.h>
#include <SDL2/SDL.h>

#include "so_util.h"

int32_t hs_SDL_DYNAPI_entry(uint32_t apiver, void *table, uint32_t tablesize);

/* liblime must be finalized (mapped) before its first SDL call. */
void sdlb_init(so_module *lime);

/* The game's window, or NULL before it exists. */
SDL_Window *sdlb_window(void);

/* Has the game opened joystick 0 yet? Until it has, its joystick events go
 * nowhere, so input.c keeps announcing the pad. */
int sdlb_game_opened_joystick(void);
int sdlb_game_opened_gamepad(void);
void sdlb_note_pad_query(const char *what);
SDL_JoystickID sdlb_joystick_instance(void);

#endif
