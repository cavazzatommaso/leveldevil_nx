/* savecache.c -- write-behind for the game's save files.
 *
 * Defold's sys.save writes a temporary file, closes it and renames it over
 * the real one; Level Devil saves "save" and "savebackup" that way on every
 * death and level change. On the Switch's SD card that is 17 filesystem
 * calls, about 95 ms and sometimes 350 ms, all on the engine thread -- the
 * stutter the game was showing (measured with frame_stats = 2).
 *
 * So files written inside save/ go to memory: fopen(.., "w") gives an
 * open_memstream, fclose stores the bytes, rename moves them to the real
 * name. All of that returns at once. A background thread then writes each
 * changed file to the card atomically (name.swtmp, then rename over the
 * real one). Reads of a file this session wrote come from memory, so the
 * game always sees its latest save; anything else reads the card as usual.
 * savecache_flush() runs at exit so nothing pending is lost.
 *
 * ponytail: one global lock and a linear table -- the game has a handful of
 * small save files. A save that is cut off by a crash or power loss in the
 * ~100 ms before the writer reaches it falls back to the previous one.
 *
 * MIT licensed, see LICENSE.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "log.h"
#include "paths.h"
#include "savecache.h"

#define SC_MAX 32

typedef struct {
    int    used;
    char   path[512];        /* device-less, as the game names it */
    char  *data;             /* NULL: deleted */
    size_t len;
    int    dirty;            /* not yet on the card */
    int    written;          /* the writer has put some version on the card */
    u64    changed;          /* tick of the last change */
} Entry;

typedef struct {
    FILE  *f;
    char   path[512];
    char  *buf;
    size_t len;
} Writer;

static Entry   g_e[SC_MAX];
static Writer  g_w[8];
static struct { FILE *f; char *copy; } g_r[8];   /* open reads of cached files */
static Mutex   g_lock;
static CondVar g_cv;
static Thread  g_thread;
static int     g_started, g_busy, g_flushing;

/* A file is written once it has been left alone this long. sys.save renames
 * its temporary file microseconds after closing it, so temporary names never
 * reach the card, and a burst of saves costs one write. */
#define SETTLE_NS 150000000ULL

int savecache_handles(const char *p)
{
    const char *dir = paths_save_nodev();
    size_t n = strlen(dir);
    return dir[0] && !strncmp(p, dir, n) && p[n] == '/';
}

static Entry *find(const char *p)
{
    int i;
    for (i = 0; i < SC_MAX; i++)
        if (g_e[i].used && !strcmp(g_e[i].path, p))
            return &g_e[i];
    return NULL;
}

static Entry *find_or_add(const char *p)
{
    Entry *e = find(p);
    int i;
    if (e)
        return e;
    for (i = 0; i < SC_MAX; i++) {
        if (!g_e[i].used) {
            memset(&g_e[i], 0, sizeof(g_e[i]));
            g_e[i].used = 1;
            snprintf(g_e[i].path, sizeof(g_e[i].path), "%s", p);
            return &g_e[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------- the writer thread -- */

static void write_one(const char *path, const char *data, size_t len)
{
    char tmp[600];
    FILE *f;

    if (!data) {
        unlink(path);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.swtmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        LOGE("savecache: cannot write %s (errno %d)", tmp, errno);
        return;
    }
    if (len && fwrite(data, 1, len, f) != len)
        LOGE("savecache: short write to %s", tmp);
    fclose(f);
    unlink(path);                       /* FAT will not rename over a file */
    if (rename(tmp, path) != 0)
        LOGE("savecache: rename %s -> %s failed (errno %d)", tmp, path, errno);
}

static void writer_main(void *arg)
{
    (void)arg;
    for (;;) {
        char path[512];
        char *data = NULL;
        size_t len = 0;
        int i, deleted = 0;
        Entry *pick = NULL;

        mutexLock(&g_lock);
        g_busy = 0;
        for (;;) {
            u64 now = armGetSystemTick(), wait_ns = 0;
            for (i = 0; i < SC_MAX && !pick; i++) {
                Entry *e = &g_e[i];
                u64 age;
                if (!e->used || !e->dirty)
                    continue;
                age = armTicksToNs(now - e->changed);
                if (g_flushing || age >= SETTLE_NS)
                    pick = e;
                else if (!wait_ns || SETTLE_NS - age < wait_ns)
                    wait_ns = SETTLE_NS - age;
            }
            if (pick)
                break;
            condvarWakeAll(&g_cv);                  /* tell savecache_flush where we are */
            if (wait_ns)
                condvarWaitTimeout(&g_cv, &g_lock, wait_ns);
            else
                condvarWait(&g_cv, &g_lock);
        }
        pick->dirty = 0;
        pick->written = 1;
        snprintf(path, sizeof(path), "%s", pick->path);
        deleted = !pick->data;
        if (!deleted && (data = malloc(pick->len ? pick->len : 1)) != NULL) {
            memcpy(data, pick->data, pick->len);
            len = pick->len;
        }
        g_busy = 1;
        mutexUnlock(&g_lock);

        if (deleted || data) {
            write_one(path, deleted ? NULL : data, len);
            LOGD("savecache: %s %s (%zu bytes)", deleted ? "deleted" : "wrote", path, len);
        }
        free(data);
        if (deleted) {
            /* Gone from the card too: the marker has done its job. */
            Entry *e;
            mutexLock(&g_lock);
            e = find(path);
            if (e && !e->data && !e->dirty)
                e->used = 0;
            mutexUnlock(&g_lock);
        }
    }
}

static void start(void)
{
    if (g_started)
        return;
    g_started = 1;
    if (R_FAILED(threadCreate(&g_thread, writer_main, NULL, NULL, 64 * 1024, 0x30, -2)) ||
        R_FAILED(threadStart(&g_thread))) {
        LOGE("savecache: writer thread failed; saves will be written on close");
        g_started = -1;
    }
}

static void mark_dirty(Entry *e)
{
    e->dirty = 1;
    e->changed = armGetSystemTick();
    if (g_started < 0) {                         /* no thread: write now */
        e->dirty = 0;
        write_one(e->path, e->data, e->len);
        return;
    }
    condvarWakeAll(&g_cv);
}

/* ----------------------------------------------------- the stdio side --- */

FILE *savecache_open_write(const char *p)
{
    int i;
    mutexLock(&g_lock);
    start();
    for (i = 0; i < (int)(sizeof(g_w) / sizeof(g_w[0])); i++) {
        if (!g_w[i].f) {
            g_w[i].buf = NULL;
            g_w[i].len = 0;
            g_w[i].f = open_memstream(&g_w[i].buf, &g_w[i].len);
            if (g_w[i].f)
                snprintf(g_w[i].path, sizeof(g_w[i].path), "%s", p);
            mutexUnlock(&g_lock);
            return g_w[i].f;
        }
    }
    mutexUnlock(&g_lock);
    return NULL;                                   /* caller falls back to the card */
}

/* Only call for a path savecache_stat reported as cached (1). */
FILE *savecache_open_read(const char *p)
{
    Entry *e;
    FILE *f = NULL;
    int i;

    mutexLock(&g_lock);
    e = find(p);
    for (i = 0; e && e->data && i < (int)(sizeof(g_r) / sizeof(g_r[0])); i++) {
        if (g_r[i].f)
            continue;
        /* fmemopen reads the buffer in place: give the stream its own copy,
         * since a later save may replace e->data while it is open. */
        g_r[i].copy = malloc(e->len ? e->len : 1);
        if (!g_r[i].copy)
            break;
        memcpy(g_r[i].copy, e->data, e->len);
        f = fmemopen(g_r[i].copy, e->len ? e->len : 1, "rb");
        if (f && !e->len)
            fseek(f, 0, SEEK_END);              /* an empty file reads as empty */
        if (f)
            g_r[i].f = f;
        else
            free(g_r[i].copy);
        break;
    }
    mutexUnlock(&g_lock);
    return f;
}

int savecache_close(FILE *f, int *ret)
{
    int i;
    mutexLock(&g_lock);
    for (i = 0; i < (int)(sizeof(g_r) / sizeof(g_r[0])); i++) {
        if (g_r[i].f == f) {
            *ret = fclose(f);
            free(g_r[i].copy);
            g_r[i].f = NULL;
            g_r[i].copy = NULL;
            mutexUnlock(&g_lock);
            return 1;
        }
    }
    for (i = 0; i < (int)(sizeof(g_w) / sizeof(g_w[0])); i++) {
        if (g_w[i].f == f) {
            Entry *e;
            *ret = fclose(f);                     /* finalizes buf/len */
            g_w[i].f = NULL;
            e = find_or_add(g_w[i].path);
            if (e) {
                free(e->data);
                e->data = g_w[i].buf;
                e->len = g_w[i].len;
                mark_dirty(e);
            } else {
                LOGE("savecache: table full, writing %s directly", g_w[i].path);
                write_one(g_w[i].path, g_w[i].buf, g_w[i].len);
                free(g_w[i].buf);
            }
            mutexUnlock(&g_lock);
            return 1;
        }
    }
    mutexUnlock(&g_lock);
    return 0;
}

int savecache_rename(const char *a, const char *b)
{
    Entry *src, *dst;
    mutexLock(&g_lock);
    src = find(a);
    if (!src || !src->data) {
        mutexUnlock(&g_lock);
        return 0;                                  /* not ours: real rename */
    }
    dst = find_or_add(b);
    if (!dst) {
        mutexUnlock(&g_lock);
        return 0;
    }
    free(dst->data);
    dst->data = src->data;
    dst->len = src->len;
    mark_dirty(dst);
    if (src->written) {
        /* The writer got to the temporary name first: remove it from the
         * card too, as the real rename would have. */
        src->data = NULL;
        src->len = 0;
        mark_dirty(src);
    } else {
        src->used = 0;                             /* never reached the card */
    }
    mutexUnlock(&g_lock);
    return 1;
}

void savecache_unlink(const char *p)
{
    Entry *e;
    mutexLock(&g_lock);
    e = find(p);
    if (e) {
        free(e->data);
        e->data = NULL;
        e->len = 0;
        mark_dirty(e);
    }
    mutexUnlock(&g_lock);
}

int savecache_stat(const char *p, size_t *size)
{
    Entry *e;
    int r = 0;
    mutexLock(&g_lock);
    e = find(p);
    if (e) {
        r = e->data ? 1 : -1;
        if (e->data && size)
            *size = e->len;
    }
    mutexUnlock(&g_lock);
    return r;
}

void savecache_flush(void)
{
    u64 until = armGetSystemTick() + armNsToTicks(3000000000ULL);
    int pending = 1;

    if (g_started <= 0)
        return;
    mutexLock(&g_lock);
    g_flushing = 1;
    condvarWakeAll(&g_cv);
    while (armGetSystemTick() < until) {
        int i;
        pending = g_busy;
        for (i = 0; i < SC_MAX; i++)
            if (g_e[i].used && g_e[i].dirty)
                pending = 1;
        if (!pending)
            break;
        condvarWaitTimeout(&g_cv, &g_lock, 50000000ULL);
    }
    g_flushing = 0;
    mutexUnlock(&g_lock);
    LOGI("savecache: %s", pending ? "gave up waiting for pending saves" : "all saves on the card");
}
