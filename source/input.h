/* input.h -- controller, keyboard and touch input, synthesized from libnx.
 *
 * WHAT THE GAME EXPECTS
 * Level Devil's Stencyl input layer reads a keyboard, a mouse and a touch
 * screen. Its com.stencyl.Input registers exactly these events -- keyDown,
 * keyUp, mouseDown, mouseUp, touchBegin, touchMove, touchEnd and a swipe
 * gesture -- and has no joystick or gamepad state at all, so a controller is
 * not something this build can read on any code path.
 *
 * Its keyboard control map is the game's own, carried inside the binary as
 * config/game-config.json:
 *
 *   Left/Right/Up/Down  arrow keys      Jump  X       Swap  C
 *   Enter  ENTER        Escape  ESCAPE  Restart  R
 *   Mute Music  M       Mute Sound  N   Fullscreen  F11
 *
 * WHAT switch-sdl2 PROVIDES
 * 28 buttons in Nintendo order, 4 axes, no hats, 8 always-present devices,
 * no keyboard, and no JOYDEVICEADDED event ever. Its touch driver also
 * mislabels finger IDs on motion.
 *
 * WHAT THIS DOES
 * sdl_bridge.c drops SDL's own joystick, controller, finger and touch-mouse
 * events. This file reads the pad and the touch screen through libnx and
 * pushes what the game can actually read:
 *
 *   - key events for the control each Switch input is mapped to (config.txt),
 *     which is what makes the pad work in the menus as well as in a level;
 *   - correct finger events for the real touch screen, and (like SDL on
 *     Android) a mouse click for the first finger;
 *   - an on-screen cursor on ZL+ZR for anything the mapping does not reach.
 *
 * It also still announces a joystick and a game controller, in the XInput
 * shape the game's own control database is written against. Nothing in this
 * build listens, but the announcement costs nothing, and the log lines say
 * whether anything ever took it.
 *
 *   button_layout = label     A->0 B->1 X->2 Y->3
 *   button_layout = position  B->0 A->1 Y->2 X->3
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_INPUT_H
#define HS_INPUT_H

#include <SDL2/SDL.h>

void input_init(void);
/* After SDL_Init / SDL_InitSubSystem: announce joystick 0 once. */
void input_on_sdl_init(void);
/* Poll libnx and push changed state as SDL events. Cheap; call often. */
void input_pump(void);
/* True while this thread is pushing a synthesized event. */
int  input_is_injecting(void);

/* Joystick queries for instance 0 are answered from the synthesized state. */
int  input_is_virtual(SDL_Joystick *joy);
int  input_num_buttons(void);
int  input_num_axes(void);
int  input_num_hats(void);
Uint8  input_button(int button);
Sint16 input_axis(int axis);
Uint8  input_hat(int hat);

/* Rumble for Android vibrate(ms). */
void input_vibrate(int ms);

#endif
