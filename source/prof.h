/* prof.h -- see prof.c. MIT licensed, see LICENSE. */
#ifndef LD_PROF_H
#define LD_PROF_H

#include <stdint.h>

enum { PX_MMAP_HINT, PX_MMAP, PX_SHADER, PX_LINK, PX_TEX, PX_BUF, PX_ASSET, PX_FILE, PX_COUNT };

/* Time one expression as file I/O when profiling. */
#define PX_FILE_OP(expr) ({                                  \
        uint64_t px_t0_ = prof_on() ? prof_now() : 0;          \
        __typeof__(expr) px_r_ = (expr);                       \
        if (px_t0_)                                            \
            prof_add(PX_FILE, px_t0_, 0);                      \
        px_r_;                                                 \
    })

/* 1 when frame_stats = 2: callers skip their timing otherwise. */
int      prof_on(void);
uint64_t prof_now(void);                       /* ticks */
void     prof_add(int kind, uint64_t t0, uint64_t bytes);   /* t0 from prof_now */

/* At every presented frame: cpu = engine time before the swap, swap = time
 * inside eglSwapBuffers, both in microseconds. Logs the frame if slow. */
void     prof_frame(uint64_t cpu_us, uint64_t swap_us);

void px_glCompileShader(unsigned shader);
void px_glLinkProgram(unsigned program);
void px_glTexImage2D(unsigned target, int level, int ifmt, int w, int h, int border,
                     unsigned fmt, unsigned type, const void *data);
void px_glTexSubImage2D(unsigned target, int level, int x, int y, int w, int h,
                        unsigned fmt, unsigned type, const void *data);
void px_glCompressedTexImage2D(unsigned target, int level, unsigned ifmt, int w, int h,
                               int border, int size, const void *data);
void px_glBufferData(unsigned target, long size, const void *data, unsigned usage);
void px_glBufferSubData(unsigned target, long off, long size, const void *data);

#endif
