/* native_activity.h -- see native_activity.c. MIT licensed, see LICENSE. */
#ifndef LD_NATIVE_ACTIVITY_H
#define LD_NATIVE_ACTIVITY_H

#include "so_util.h"

/* ANativeActivity_onCreate and the start-up lifecycle, on the calling thread.
 * Returns 0 if the engine has no NativeActivity entry point. */
int  na_start(so_module *engine);
/* Dock/undock: the window changed size. */
void na_window_resized(void);
/* HOME menu in/out. */
void na_focus(int has_focus);

/* Implemented in main.c: the game asked to close. */
void na_on_finish(void);

void ax_ANativeActivity_finish(void *activity);

#endif
