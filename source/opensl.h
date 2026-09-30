/* opensl.h -- see opensl.c. MIT licensed, see LICENSE. */
#ifndef LD_OPENSL_H
#define LD_OPENSL_H

#include <stdint.h>

extern const void *ax_SL_IID_ENGINE;
extern const void *ax_SL_IID_BUFFERQUEUE;
extern const void *ax_SL_IID_PLAY;
extern const void *ax_SL_IID_VOLUME;
extern const void *ax_SL_IID_ANDROIDSIMPLEBUFFERQUEUE;

uint32_t ax_slCreateEngine(void **engine, uint32_t nopts, const void *opts,
                           uint32_t nifaces, const void *ids, const void *req);

#endif
