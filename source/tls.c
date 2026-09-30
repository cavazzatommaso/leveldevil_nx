/* tls.c -- a bionic thread pointer, so the game's stack checks can read it.
 *
 * WHAT WENT WRONG WITHOUT THIS
 *
 * The game reached hxcpp_main and died immediately on:
 *
 *     mrs  x19, tpidr_el0
 *     ldr  x8, [x19, #0x28]     <- data abort, x19 was 0
 *
 * That is the stack-protector canary being read. Android's C runtime keeps
 * it in a thread-local slot rather than a global: TPIDR_EL0 points at the
 * thread's TLS block and TLS_SLOT_STACK_GUARD is slot 5, which is byte
 * offset 5 * 8 = 0x28. Every function the game compiled with
 * -fstack-protector reads it on entry and checks it on return, so there is
 * no patching around it -- there are thousands of them.
 *
 * Total Party Kill never hit this because its older NDK imported
 * __stack_chk_guard as an ordinary global, which imports.c could simply
 * provide. This build imports no such symbol; it reads TLS directly.
 *
 * WHY THIS IS SAFE ON THE SWITCH
 *
 * devkitA64 is built with soft thread-pointer access: the compiler emits a
 * call to __aarch64_read_tp, and libnx implements that by reading
 * TPIDRRO_EL0 (nx/source/runtime/readtp.s). Checked against a real build of
 * this port: 204 references to tpidrro_el0 and not one to tpidr_el0. So
 * TPIDR_EL0 is untouched by the host runtime and free for the modules, and
 * the host's own __thread variables keep working while this is installed --
 * which is what makes the "already attached" flag below usable.
 *
 * The modules need the slots, not real TLS: neither library has a PT_TLS
 * segment and neither imports __tls_get_addr or __emutls_get_address.
 *
 * EVERY thread that can run module code needs this: the main thread (it runs
 * the modules' constructors), the game thread, the pump thread, and every
 * thread the modules create themselves through the pthread shim -- hxcpp's
 * GC workers and SDL's audio thread among them.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "tls.h"

/* bionic's slot numbering (bionic/libc/private/bionic_tls.h). Only the guard
 * is known to be read by this game, but the block is sized generously: a
 * wrong guess here is a fault inside the game with no symbol to blame. */
#define TLS_SLOT_SELF          0
#define TLS_SLOT_THREAD_ID     1
#define TLS_SLOT_APP           2
#define TLS_SLOT_OPENGL        3
#define TLS_SLOT_OPENGL_API    4
#define TLS_SLOT_STACK_GUARD   5

/* The block is bigger than the slots need and the thread pointer is placed
 * part-way into it. bionic reaches some of its own thread state at NEGATIVE
 * offsets from the thread pointer, so a block that starts exactly at the
 * pointer has another allocation immediately below it -- and a stray write
 * lands in someone else's heap. Confirmed against a working Switch port of a
 * Unity title, which uses the same 0x400 block with the pointer at +0x200. */
#define PB_TLS_BLOCK_SIZE   0x400
#define PB_TLS_TP_OFFSET    0x200

/* One canary for the whole process. It only has to be stable between a
 * function's prologue and its epilogue; a per-thread value would work too,
 * but a single one also survives a frame that is set up on one thread and
 * unwound on another. */
static u64 g_canary;

/* Host TLS, reached through TPIDRRO_EL0, so it is readable whatever
 * TPIDR_EL0 currently holds -- including when it is still 0. */
static __thread void *t_block;      /* the allocation */
static __thread void *t_tp;         /* what TPIDR_EL0 was set to */

static inline void write_tp(void *p)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(p));
}

static inline void *read_tp(void)
{
    void *p;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p));
    return p;
}

void pb_tls_init(void)
{
    if (g_canary)
        return;
    /* Not cryptographic, and it does not need to be: nothing here defends
     * against an attacker, it just has to be a stable non-zero value. */
    g_canary = (armGetSystemTick() * 0x9E3779B97F4A7C15ULL) | 0x0101000000000001ULL;
}

int pb_tls_attach(const char *who)
{
    void *block, **tp;

    if (t_block)
        return 1;                    /* this thread already has one */

    pb_tls_init();
    block = calloc(1, PB_TLS_BLOCK_SIZE);
    if (!block) {
        LOGE("tls: out of memory for %s's thread block; the game will fault "
             "on its first stack check", who ? who : "a thread");
        return 0;
    }
    tp = (void **)((char *)block + PB_TLS_TP_OFFSET);
    tp[TLS_SLOT_SELF] = tp;
    tp[TLS_SLOT_THREAD_ID] = (void *)(uintptr_t)threadGetCurHandle();
    tp[TLS_SLOT_STACK_GUARD] = (void *)(uintptr_t)g_canary;

    t_block = block;
    t_tp = tp;
    write_tp(tp);
    LOGD("tls: thread pointer installed for %s (tp %p in block %p)",
         who ? who : "?", (void *)tp, block);
    return 1;
}

void pb_tls_detach(void)
{
    void *block = t_block;

    if (!block)
        return;
    if (read_tp() == t_tp)
        write_tp(NULL);
    t_block = NULL;
    t_tp = NULL;
    free(block);
}

int pb_tls_present(void)
{
    return t_block != NULL;
}

/* Read the guard back the way the game's own prologues do, so the log says
 * plainly whether the slot the crash was about is now readable. */
int pb_tls_selftest(void)
{
    u64 seen;
    void *tp = read_tp();

    if (!tp) {
        LOGE("tls: selftest: TPIDR_EL0 is still 0");
        return 0;
    }
    seen = *(volatile u64 *)((char *)tp + TLS_SLOT_STACK_GUARD * 8);
    if (seen != g_canary) {
        LOGE("tls: selftest: guard slot holds 0x%016llx, expected 0x%016llx",
             (unsigned long long)seen, (unsigned long long)g_canary);
        return 0;
    }
    LOGI("tls: TPIDR_EL0 = %p, stack guard at +0x28 reads back correctly",
         tp);
    return 1;
}
