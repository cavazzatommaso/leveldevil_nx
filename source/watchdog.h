/* watchdog.h -- see watchdog.c. MIT licensed, see LICENSE. */
#ifndef PB_WATCHDOG_H
#define PB_WATCHDOG_H
void     watchdog_start(void);
void     watchdog_stop(void);
/* Called from the interposed eglSwapBuffers: one presented frame. */
void     watchdog_note_swap(void);
unsigned watchdog_swaps(void);
#endif
