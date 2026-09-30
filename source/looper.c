/* looper.c -- pipes, ALooper and AInputQueue: the event plumbing under
 * Defold's android_native_app_glue.
 *
 * The glue (compiled into libLevelDevil.so) works like this on a phone:
 *
 *   ANativeActivity_onCreate  pipe(msgpipe), start the app thread, wait
 *   app thread                ALooper_prepare; ALooper_addFd(msgread, MAIN);
 *                             android_main() -- the engine -- which calls
 *                             ALooper_pollOnce every frame
 *   activity callbacks        write one command byte into msgpipe and block
 *                             until the app thread has handled it
 *   onInputQueueCreated       the app thread attaches the queue to its looper
 *                             and drains AInputQueue_getEvent when polled
 *
 * The Switch has no pipes and no epoll, so both halves are here: a pipe is a
 * small in-process ring with fds from LP_FD_BASE up (bionic_stdio.c routes
 * read/write/close for those fds here), and a looper is a list of those fds
 * plus the input queue, woken by one process-wide condition variable. Every
 * write and every queued input event broadcasts it.
 *
 * MIT licensed, see LICENSE.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "log.h"
#include "looper.h"

#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_POLL_ERROR    (-4)
#define ALOOPER_EVENT_INPUT   1

#define AINPUT_EVENT_TYPE_KEY    1
#define AINPUT_EVENT_TYPE_MOTION 2

static Mutex   g_lock;
static CondVar g_cv;

/* ------------------------------------------------------------- pipes ---- */

#define LP_PIPES   16
#define LP_PIPESZ  4096

typedef struct {
    int      used, rd_open, wr_open;
    size_t   head, len;
    uint8_t  buf[LP_PIPESZ];
} LpPipe;

static LpPipe g_pipes[LP_PIPES];

static LpPipe *pipe_of(int fd, int *is_write)
{
    int i = fd - LP_FD_BASE;
    if (i < 0 || i >= LP_PIPES * 2 || !g_pipes[i / 2].used)
        return NULL;
    if (is_write)
        *is_write = i & 1;
    return &g_pipes[i / 2];
}

int lp_is_fd(int fd) { return fd >= LP_FD_BASE && fd < LP_FD_BASE + LP_PIPES * 2; }

int lp_pipe(int fds[2])
{
    int i;
    mutexLock(&g_lock);
    for (i = 0; i < LP_PIPES; i++) {
        if (!g_pipes[i].used) {
            memset(&g_pipes[i], 0, sizeof(g_pipes[i]));
            g_pipes[i].used = g_pipes[i].rd_open = g_pipes[i].wr_open = 1;
            fds[0] = LP_FD_BASE + i * 2;
            fds[1] = LP_FD_BASE + i * 2 + 1;
            mutexUnlock(&g_lock);
            LOGD("pipe: %d/%d", fds[0], fds[1]);
            return 0;
        }
    }
    mutexUnlock(&g_lock);
    errno = LX_EMFILE;
    return -1;
}

/* Readable = has data, or the writer is gone (read then returns 0). */
static int pipe_readable(const LpPipe *p) { return p->len > 0 || !p->wr_open; }

long lp_read(int fd, void *buf, size_t n)
{
    int w;
    LpPipe *p = pipe_of(fd, &w);
    size_t got = 0;

    if (!p || w) {
        errno = LX_EBADF;
        return -1;
    }
    mutexLock(&g_lock);
    while (!pipe_readable(p))                   /* blocking, like a real pipe */
        condvarWait(&g_cv, &g_lock);
    while (got < n && p->len) {
        ((uint8_t *)buf)[got++] = p->buf[p->head];
        p->head = (p->head + 1) % LP_PIPESZ;
        p->len--;
    }
    condvarWakeAll(&g_cv);                      /* room for a blocked writer */
    mutexUnlock(&g_lock);
    return (long)got;
}

long lp_write(int fd, const void *buf, size_t n)
{
    int w;
    LpPipe *p = pipe_of(fd, &w);
    size_t put = 0;

    if (!p || !w) {
        errno = LX_EBADF;
        return -1;
    }
    mutexLock(&g_lock);
    while (put < n) {
        while (p->len == LP_PIPESZ && p->rd_open)
            condvarWait(&g_cv, &g_lock);
        if (!p->rd_open)
            break;
        p->buf[(p->head + p->len) % LP_PIPESZ] = ((const uint8_t *)buf)[put++];
        p->len++;
    }
    condvarWakeAll(&g_cv);
    mutexUnlock(&g_lock);
    if (!put && n) {
        errno = 32;                             /* EPIPE */
        return -1;
    }
    return (long)put;
}

int lp_close(int fd)
{
    int w;
    LpPipe *p = pipe_of(fd, &w);
    if (!p) {
        errno = LX_EBADF;
        return -1;
    }
    mutexLock(&g_lock);
    if (w)
        p->wr_open = 0;
    else
        p->rd_open = 0;
    if (!p->rd_open && !p->wr_open)
        p->used = 0;
    condvarWakeAll(&g_cv);
    mutexUnlock(&g_lock);
    return 0;
}

/* ------------------------------------------------------- input events --- */

#define LP_MAX_POINTERS 10
#define LP_QUEUE_LEN    256

typedef struct {
    int32_t type, source, device, action, keycode;
    int64_t down_time, time;
    int32_t count;
    int32_t ids[LP_MAX_POINTERS];
    float   x[LP_MAX_POINTERS], y[LP_MAX_POINTERS];
} LpEvent;

typedef struct {
    void    *looper;
    int      ident;
    void    *data;
    LpEvent *ring[LP_QUEUE_LEN];
    int      head, len;
} LpQueue;

static LpQueue g_queue;

void *lp_input_queue(void) { return &g_queue; }

static int64_t now_ns(void) { return (int64_t)armTicksToNs(armGetSystemTick()); }

static void enqueue(LpEvent *e)
{
    mutexLock(&g_lock);
    if (g_queue.len == LP_QUEUE_LEN) {
        LOG_ONCE("input: queue full, dropping events (is the game polling?)");
        free(e);
    } else {
        g_queue.ring[(g_queue.head + g_queue.len) % LP_QUEUE_LEN] = e;
        g_queue.len++;
        condvarWakeAll(&g_cv);
    }
    mutexUnlock(&g_lock);
}

void lp_push_key(int keycode, int down)
{
    static int64_t down_at[512];
    LpEvent *e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->type = AINPUT_EVENT_TYPE_KEY;
    e->source = LP_SOURCE_KEYBOARD;
    e->device = 1;
    e->action = down ? LP_KEY_DOWN : LP_KEY_UP;
    e->keycode = keycode;
    e->time = now_ns();
    if (keycode >= 0 && keycode < 512) {
        if (down)
            down_at[keycode] = e->time;
        e->down_time = down_at[keycode];
    }
    enqueue(e);
}

void lp_push_motion(int action, int count, const int *ids, const float *xs, const float *ys)
{
    static int64_t down_at;
    LpEvent *e = calloc(1, sizeof(*e));
    int i;
    if (!e)
        return;
    if (count > LP_MAX_POINTERS)
        count = LP_MAX_POINTERS;
    e->type = AINPUT_EVENT_TYPE_MOTION;
    e->source = LP_SOURCE_TOUCHSCREEN;
    e->device = 2;
    e->action = action;
    e->time = now_ns();
    if ((action & 0xff) == LP_MOTION_DOWN)
        down_at = e->time;
    e->down_time = down_at;
    e->count = count;
    for (i = 0; i < count; i++) {
        e->ids[i] = ids[i];
        e->x[i] = xs[i];
        e->y[i] = ys[i];
    }
    enqueue(e);
}

void ax_AInputQueue_attachLooper(void *q, void *looper, int ident, void *callback, void *data)
{
    LpQueue *iq = q;
    if (callback)
        LOG_ONCE("!! AInputQueue_attachLooper with a callback (not supported)");
    mutexLock(&g_lock);
    iq->looper = looper;
    iq->ident = ident;
    iq->data = data;
    condvarWakeAll(&g_cv);
    mutexUnlock(&g_lock);
    LOGI("input: queue attached to looper %p (ident %d)", looper, ident);
}

void ax_AInputQueue_detachLooper(void *q)
{
    mutexLock(&g_lock);
    ((LpQueue *)q)->looper = NULL;
    mutexUnlock(&g_lock);
}

int32_t ax_AInputQueue_getEvent(void *q, void **out)
{
    LpQueue *iq = q;
    int32_t r = -1;
    mutexLock(&g_lock);
    if (iq->len) {
        *out = iq->ring[iq->head];
        iq->head = (iq->head + 1) % LP_QUEUE_LEN;
        iq->len--;
        r = 0;
    }
    mutexUnlock(&g_lock);
    return r;
}

int32_t ax_AInputQueue_preDispatchEvent(void *q, void *ev) { (void)q; (void)ev; return 0; }
void    ax_AInputQueue_finishEvent(void *q, void *ev, int handled) { (void)q; (void)handled; free(ev); }

#define EV(p) ((const LpEvent *)(p))
int32_t ax_AInputEvent_getType(const void *ev)     { return EV(ev)->type; }
int32_t ax_AInputEvent_getSource(const void *ev)   { return EV(ev)->source; }
int32_t ax_AInputEvent_getDeviceId(const void *ev) { return EV(ev)->device; }
int32_t ax_AKeyEvent_getAction(const void *ev)     { return EV(ev)->action; }
int32_t ax_AKeyEvent_getKeyCode(const void *ev)    { return EV(ev)->keycode; }
int32_t ax_AKeyEvent_getScanCode(const void *ev)   { (void)ev; return 0; }
int32_t ax_AKeyEvent_getMetaState(const void *ev)  { (void)ev; return 0; }
int32_t ax_AKeyEvent_getFlags(const void *ev)      { (void)ev; return 0; }
int32_t ax_AKeyEvent_getRepeatCount(const void *ev){ (void)ev; return 0; }
int64_t ax_AKeyEvent_getDownTime(const void *ev)   { return EV(ev)->down_time; }
int64_t ax_AKeyEvent_getEventTime(const void *ev)  { return EV(ev)->time; }
int32_t ax_AMotionEvent_getAction(const void *ev)  { return EV(ev)->action; }
size_t  ax_AMotionEvent_getPointerCount(const void *ev) { return (size_t)EV(ev)->count; }

int32_t ax_AMotionEvent_getPointerId(const void *ev, size_t i)
{
    return i < (size_t)EV(ev)->count ? EV(ev)->ids[i] : 0;
}
float ax_AMotionEvent_getX(const void *ev, size_t i) { return i < (size_t)EV(ev)->count ? EV(ev)->x[i] : 0; }
float ax_AMotionEvent_getY(const void *ev, size_t i) { return i < (size_t)EV(ev)->count ? EV(ev)->y[i] : 0; }

float ax_AMotionEvent_getAxisValue(const void *ev, int32_t axis, size_t i)
{
    if (axis == 0) return ax_AMotionEvent_getX(ev, i);   /* AXIS_X */
    if (axis == 1) return ax_AMotionEvent_getY(ev, i);   /* AXIS_Y */
    return 0.0f;
}

/* ------------------------------------------------------------ ALooper --- */

#define LP_LOOPER_FDS 16

typedef struct {
    int   n;
    struct { int fd, ident, events; void *callback, *data; } fds[LP_LOOPER_FDS];
} LpLooper;

static __thread LpLooper *t_looper;

void *ax_ALooper_prepare(int opts)
{
    (void)opts;
    if (!t_looper) {
        t_looper = calloc(1, sizeof(*t_looper));
        LOGI("looper: prepared %p", (void *)t_looper);
    }
    return t_looper;
}

void *ax_ALooper_forThread(void) { return t_looper; }

int ax_ALooper_addFd(void *looper, int fd, int ident, int events, void *callback, void *data)
{
    LpLooper *l = looper;
    int i;
    if (!l)
        return -1;
    if (!lp_is_fd(fd))
        LOGI("looper: addFd(%d) is not a pipe; it will never fire", fd);
    mutexLock(&g_lock);
    for (i = 0; i < l->n && l->fds[i].fd != fd; i++)
        ;
    if (i == l->n) {
        if (l->n == LP_LOOPER_FDS) {
            mutexUnlock(&g_lock);
            return -1;
        }
        l->n++;
    }
    l->fds[i].fd = fd;
    l->fds[i].ident = ident;
    l->fds[i].events = events;
    l->fds[i].callback = callback;
    l->fds[i].data = data;
    mutexUnlock(&g_lock);
    return 1;
}

int ax_ALooper_removeFd(void *looper, int fd)
{
    LpLooper *l = looper;
    int i, r = 0;
    if (!l)
        return -1;
    mutexLock(&g_lock);
    for (i = 0; i < l->n; i++) {
        if (l->fds[i].fd == fd) {
            l->fds[i] = l->fds[--l->n];
            r = 1;
            break;
        }
    }
    mutexUnlock(&g_lock);
    return r;
}

typedef int (*LooperCb)(int fd, int events, void *data);

/* Called with g_lock held. Runs (unlocked) the callback of every ready fd
 * that has one, and returns ALOOPER_POLL_CALLBACK if any ran; otherwise the
 * ident of the first ready callback-less fd or of the input queue, or
 * ALOOPER_POLL_TIMEOUT if nothing is ready. */
static int ready(LpLooper *l, int *fd, int *events, void **data)
{
    struct { LooperCb cb; int fd; void *data; } run[LP_LOOPER_FDS];
    int i, nrun = 0;

    for (i = 0; i < l->n; i++) {
        LpPipe *p = pipe_of(l->fds[i].fd, NULL);
        if (!p || !pipe_readable(p))
            continue;
        if (l->fds[i].callback) {
            run[nrun].cb = (LooperCb)l->fds[i].callback;
            run[nrun].fd = l->fds[i].fd;
            run[nrun].data = l->fds[i].data;
            nrun++;
            continue;
        }
        if (!nrun) {
            if (fd)     *fd = l->fds[i].fd;
            if (events) *events = ALOOPER_EVENT_INPUT;
            if (data)   *data = l->fds[i].data;
            return l->fds[i].ident;
        }
    }
    if (nrun) {
        /* Each ready callback runs once per poll, unlocked; a callback that
         * returns 0 is unregistered, as on Android. */
        for (i = 0; i < nrun; i++) {
            int keep;
            mutexUnlock(&g_lock);
            keep = run[i].cb(run[i].fd, ALOOPER_EVENT_INPUT, run[i].data);
            mutexLock(&g_lock);
            if (!keep) {
                int j;
                for (j = 0; j < l->n; j++)
                    if (l->fds[j].fd == run[i].fd) {
                        l->fds[j] = l->fds[--l->n];
                        break;
                    }
            }
        }
        return ALOOPER_POLL_CALLBACK;
    }
    if (g_queue.looper == l && g_queue.len) {
        if (fd)     *fd = -1;
        if (events) *events = ALOOPER_EVENT_INPUT;
        if (data)   *data = g_queue.data;
        return g_queue.ident;
    }
    return ALOOPER_POLL_TIMEOUT;
}

int ax_ALooper_pollOnce(int timeout, int *fd, int *events, void **data)
{
    LpLooper *l = t_looper;
    u64 deadline = 0;
    int r;

    if (!l) {
        LOG_ONCE("!! ALooper_pollOnce on a thread with no looper");
        return ALOOPER_POLL_ERROR;
    }
    if (timeout > 0)
        deadline = armGetSystemTick() + armNsToTicks((u64)timeout * 1000000ULL);

    mutexLock(&g_lock);
    for (;;) {
        r = ready(l, fd, events, data);
        if (r != ALOOPER_POLL_TIMEOUT || timeout == 0)
            break;
        if (timeout < 0) {
            condvarWaitTimeout(&g_cv, &g_lock, 100000000ULL);
        } else {
            u64 now = armGetSystemTick();
            if (now >= deadline)
                break;
            condvarWaitTimeout(&g_cv, &g_lock, armTicksToNs(deadline - now));
        }
    }
    mutexUnlock(&g_lock);
    return r;
}

int ax_ALooper_pollAll(int timeout, int *fd, int *events, void **data)
{
    return ax_ALooper_pollOnce(timeout, fd, events, data);
}
