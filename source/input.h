/* input.h -- see input.c. MIT licensed, see LICENSE. */
#ifndef PB_INPUT_H
#define PB_INPUT_H

void input_init(void);
/* Read the pad and the touch screen, queue Android input events. Called from
 * the main loop about every 8 ms. */
void input_pump(void);

#endif
