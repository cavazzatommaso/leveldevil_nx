/* app.h -- process lifecycle shared by the shims.
 *
 * libnx's exit path has to run on the main thread (it restores the loader's
 * context), but the game asks to exit from its own thread: exit() from hxcpp's
 * Sys.exit, abort(), Activity.moveTaskToBack from Lime's System.exit, the
 * HOME-menu close request, or the + and - combo. All of those funnel into
 * hs_request_exit(), which wakes the main thread and parks the caller.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_APP_H
#define HS_APP_H

void hs_request_exit(int code);
int  hs_exit_requested(void);
int  hs_exit_code(void);
void hs_park_forever(void) __attribute__((noreturn));

/* The game has been handed SDL_QUIT. If it has not exited a few seconds
 * later (a stuck shutdown), the main thread exits anyway. */
void hs_note_quit(void);

/* Seconds since start, for rate-limited logging. */
double hs_uptime(void);

#endif
