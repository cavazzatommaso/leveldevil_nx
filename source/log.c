/* log.c -- see log.h. MIT licensed, see LICENSE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "log.h"

static FILE  *g_fp;
static volatile int g_crashing;   /* set by the crash handler */
static char  *g_buf;
static int    g_level = PB_LOG_INFO;
static RMutex g_lock;
static u64    g_tick0;
static int    g_dirty;

static char g_path[512];

/* Opened on the first line actually written, so a run with logging off leaves
 * no file behind. */
static void open_file(void)
{
    if (g_fp || !g_path[0])
        return;
    g_fp = fopen(g_path, "w");
    if (!g_fp)
        return;
    g_buf = malloc(1 << 16);
    if (g_buf)
        setvbuf(g_fp, g_buf, _IOFBF, 1 << 16);
    fputs("Level Devil (Defold) -- Nintendo Switch wrapper port\n", g_fp);
}

void log_init(const char *path)
{
    rmutexInit(&g_lock);
    g_tick0 = armGetSystemTick();
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
}

void log_set_level(int level) { g_level = level; }
int  log_get_level(void)      { return g_level; }

void log_vprintf(const char *fmt, va_list ap)
{
    char line[2048];
    int n, pre;
    u64 ms;

    if (g_crashing || g_level <= PB_LOG_OFF)
        return;

    ms = armTicksToNs(armGetSystemTick() - g_tick0) / 1000000ULL;
    pre = snprintf(line, sizeof(line), "[%5lu.%03lu] ",
                   (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    n = vsnprintf(line + pre, sizeof(line) - pre - 1, fmt, ap);
    if (n < 0)
        return;
    n += pre;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    while (n > pre && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    line[n++] = '\n';

    rmutexLock(&g_lock);
    open_file();
    if (!g_fp) {
        rmutexUnlock(&g_lock);
        return;
    }
    fwrite(line, 1, (size_t)n, g_fp);
    g_dirty = 1;
    if (line[pre] == '!' && line[pre + 1] == '!') {
        fflush(g_fp);
        g_dirty = 0;
    }
    rmutexUnlock(&g_lock);
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
}

void log_flush(void)
{
    /* Writing to the SD card can block for milliseconds, so this is only
     * worth doing when there is something buffered -- and the main thread,
     * not the game thread, is the one that calls it on a timer. */
    if (g_crashing || !g_fp || !g_dirty)
        return;
    rmutexLock(&g_lock);
    if (g_dirty) {
        fflush(g_fp);
        g_dirty = 0;
    }
    rmutexUnlock(&g_lock);
}

/* One byte longer than anything written into it, and that byte is never
 * touched, so a reader that races a write still sees a terminated string.
 * log_last_stage() is deliberately lock-free: the crash handler calls it and
 * must never block on a lock the faulting thread might hold. */
static char g_stage[80] = "(nothing yet)";

/* Once the crash handler owns the file, everyone else keeps away from it. */
void log_begin_crash(void) { g_crashing = 1; }

void log_stage(const char *name)
{
    if (!name || g_crashing)
        return;
    /* Under the same lock as every other writer. An earlier version wrote to
     * g_fp without it, and the main thread's stage line duly interleaved into
     * a crash report from another thread -- newlib's FILE has no internal
     * locking here. g_crashing is checked before the lock so a faulting
     * thread holding it cannot deadlock the others. */
    rmutexLock(&g_lock);
    /* The breadcrumb is always kept, whatever the log level: it costs one
     * small copy and it is what lets a crash report say how far start-up
     * got even on a release build with logging off. Only the FILE write is
     * gated. */
    snprintf(g_stage, sizeof(g_stage) - 1, "%s", name);
    if (g_level > PB_LOG_OFF) {
        open_file();
        if (g_fp) {
            fprintf(g_fp, "[%8.3f] stage: %s\n", pb_uptime(), g_stage);
            fflush(g_fp);
        }
    }
    rmutexUnlock(&g_lock);
}

const char *log_last_stage(void) { return g_stage; }

void log_emergency(const char *s)
{
    open_file();          /* a crash report is worth a file even with logging off */
    /* No lock: the thread that faulted may be holding it. A torn line in the
     * crash report beats a deadlock that produces no report at all. */
    if (!g_fp)
        return;
    fputs(s, g_fp);
    fflush(g_fp);
}
