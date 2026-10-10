/* prof.c -- frame_stats = 2: say what a slow frame was spent on.
 *
 * A slow frame (over 20 ms between two presents) is logged with how long the
 * engine worked before presenting, how long it then waited inside
 * eglSwapBuffers, and what it did meanwhile: shader compiles and links,
 * texture and buffer uploads, asset reads, and mmap calls (hinted ones are
 * LuaJIT asking for machine-code memory). Counters are reset every frame.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "prof.h"

static const char *const NAMES[PX_COUNT] = {
    "mmap-hinted", "mmap", "shader", "link", "tex", "buf", "asset", "file",
};

static _Atomic uint64_t g_ns[PX_COUNT], g_n[PX_COUNT], g_bytes[PX_COUNT];

int      prof_on(void)  { return g_cfg.frame_stats >= 2; }
uint64_t prof_now(void) { return armGetSystemTick(); }

void prof_add(int kind, uint64_t t0, uint64_t bytes)
{
    atomic_fetch_add_explicit(&g_ns[kind], armTicksToNs(armGetSystemTick() - t0), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_n[kind], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bytes[kind], bytes, memory_order_relaxed);
}

void prof_frame(uint64_t cpu_us, uint64_t swap_us)
{
    char line[512];
    int i, len;

    if (!prof_on())
        return;
    if (cpu_us + swap_us > 20000) {
        len = snprintf(line, sizeof(line), "slow frame: %llu ms (engine %llu ms, swap wait %llu ms)",
                       (unsigned long long)((cpu_us + swap_us) / 1000),
                       (unsigned long long)(cpu_us / 1000), (unsigned long long)(swap_us / 1000));
        for (i = 0; i < PX_COUNT && len < (int)sizeof(line) - 64; i++) {
            uint64_t n = atomic_load_explicit(&g_n[i], memory_order_relaxed);
            if (!n)
                continue;
            len += snprintf(line + len, sizeof(line) - (size_t)len, " | %s %llux %.1f ms %llu KB", NAMES[i],
                            (unsigned long long)n,
                            atomic_load_explicit(&g_ns[i], memory_order_relaxed) / 1e6,
                            (unsigned long long)(atomic_load_explicit(&g_bytes[i], memory_order_relaxed) / 1024));
        }
        LOGI("%s", line);
    }
    for (i = 0; i < PX_COUNT; i++) {
        atomic_store_explicit(&g_ns[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_n[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_bytes[i], 0, memory_order_relaxed);
    }
}

/* ---- GL wrappers: the real call, timed only when profiling ---- */

extern void glCompileShader(unsigned);
extern void glLinkProgram(unsigned);
extern void glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glCompressedTexImage2D(unsigned, int, unsigned, int, int, int, int, const void *);
extern void glBufferData(unsigned, long, const void *, unsigned);
extern void glBufferSubData(unsigned, long, long, const void *);

#define TIMED(kind, bytes, call) do {          \
        if (!prof_on()) { call; break; }       \
        uint64_t t0_ = prof_now();             \
        call;                                  \
        prof_add(kind, t0_, bytes);            \
    } while (0)

void px_glCompileShader(unsigned s) { TIMED(PX_SHADER, 0, glCompileShader(s)); }
void px_glLinkProgram(unsigned p)   { TIMED(PX_LINK, 0, glLinkProgram(p)); }

void px_glTexImage2D(unsigned target, int level, int ifmt, int w, int h, int border,
                     unsigned fmt, unsigned type, const void *data)
{
    TIMED(PX_TEX, (uint64_t)w * (uint64_t)h * 4u,
          glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data));
}

void px_glTexSubImage2D(unsigned target, int level, int x, int y, int w, int h,
                        unsigned fmt, unsigned type, const void *data)
{
    TIMED(PX_TEX, (uint64_t)w * (uint64_t)h * 4u,
          glTexSubImage2D(target, level, x, y, w, h, fmt, type, data));
}

void px_glCompressedTexImage2D(unsigned target, int level, unsigned ifmt, int w, int h,
                               int border, int size, const void *data)
{
    TIMED(PX_TEX, (uint64_t)size, glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data));
}

void px_glBufferData(unsigned target, long size, const void *data, unsigned usage)
{
    TIMED(PX_BUF, (uint64_t)size, glBufferData(target, size, data, usage));
}

void px_glBufferSubData(unsigned target, long off, long size, const void *data)
{
    TIMED(PX_BUF, (uint64_t)size, glBufferSubData(target, off, size, data));
}
