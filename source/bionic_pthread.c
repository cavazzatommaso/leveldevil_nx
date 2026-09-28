/* bionic_pthread.c -- bionic threads, locks and futex on libnx primitives.
 *
 * WHY NOT JUST CALL NEWLIB'S PTHREADS
 * bionic's pthread objects are int32 arrays (4-byte aligned) of a different
 * size and meaning from newlib's, and static initializers
 * (PTHREAD_MUTEX_INITIALIZER, the recursive variant) are baked into both
 * modules' data. So each bionic object carries an index into a handle table
 * in its first int32, created lazily on first use. The first word of a
 * statically initialized object reads 0 (normal) or 0x4000 / 0x8000
 * (recursive / errorcheck); handles carry the top bit so they never collide.
 *
 * Everything below the handle table is libnx directly: Mutex, RMutex, CondVar
 * with relative timeouts. That avoids newlib's CLOCK_REALTIME abstime
 * conversion and gives condition-variable waits on recursive mutexes, which
 * hxcpp uses.
 *
 * No compiler atomics: slow paths take a lock, fast paths read a word that
 * is only ever written once from 0 to its final value under that lock.
 *
 * Condition waits without a timeout are capped at 32 ms. POSIX allows
 * spurious wakeups and every caller re-checks its predicate; the cap turns
 * any lost wakeup into a 32 ms hiccup instead of a hang.
 *
 * MIT licensed, see LICENSE.
 */
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <switch.h>

#include "bionic.h"
#include "log.h"

#define HANDLE_TAG   0x80000000u
#define CHUNK_SIZE   4096
#define CHUNK_COUNT  64

#define INIT_RECURSIVE  0x4000
#define INIT_ERRORCHECK 0x8000

#define COND_WAIT_CAP_NS 32000000ULL

/* ------------------------------------------------------------ handles -- */

static void    **g_chunks[CHUNK_COUNT];
static uint32_t  g_next = 1;
static uint32_t *g_free;
static size_t    g_free_n, g_free_cap;
static Mutex     g_table_lock;

static void *handle_get(uint32_t word)
{
    uint32_t idx = word & ~HANDLE_TAG;
    void **chunk;
    /* The index comes out of a bionic lock object, so it is only ours if the
     * object really is one we initialized. A struct that was copied, freed
     * and reused, or memset to 0xff, can present a tagged word that is not an
     * index at all; without this check that reads past g_chunks. */
    if (idx >= CHUNK_SIZE * CHUNK_COUNT) {
        LOG_ONCE("pthread: ignoring a lock object with a bad handle (0x%08x)", word);
        return NULL;
    }
    chunk = g_chunks[idx / CHUNK_SIZE];
    return chunk ? chunk[idx % CHUNK_SIZE] : NULL;
}

/* Caller holds g_table_lock. Returns a tagged handle or 0. */
static uint32_t handle_add_locked(void *obj)
{
    uint32_t idx;
    if (g_free_n) {
        idx = g_free[--g_free_n];
    } else {
        if (g_next >= CHUNK_SIZE * CHUNK_COUNT)
            return 0;
        idx = g_next++;
        if (!g_chunks[idx / CHUNK_SIZE]) {
            void **c = calloc(CHUNK_SIZE, sizeof(void *));
            if (!c)
                return 0;
            g_chunks[idx / CHUNK_SIZE] = c;
        }
    }
    g_chunks[idx / CHUNK_SIZE][idx % CHUNK_SIZE] = obj;
    return idx | HANDLE_TAG;
}

static void handle_remove_locked(uint32_t word)
{
    uint32_t idx = word & ~HANDLE_TAG;
    void **chunk = g_chunks[idx / CHUNK_SIZE];
    if (!chunk)
        return;
    chunk[idx % CHUNK_SIZE] = NULL;
    if (g_free_n == g_free_cap) {
        size_t cap = g_free_cap ? g_free_cap * 2 : 256;
        uint32_t *n = realloc(g_free, cap * sizeof(uint32_t));
        if (!n)
            return;                      /* leak the slot, not fatal */
        g_free = n;
        g_free_cap = cap;
    }
    g_free[g_free_n++] = idx;
}

/* ------------------------------------------------------------ mutexes -- */

typedef struct {
    int    recursive;
    Mutex  m;
    RMutex rm;
} NxMutex;

static NxMutex *mutex_new(int recursive)
{
    NxMutex *nm = calloc(1, sizeof(*nm));
    if (!nm)
        return NULL;
    nm->recursive = recursive;
    mutexInit(&nm->m);
    rmutexInit(&nm->rm);
    return nm;
}

static NxMutex *mutex_get(bionic_mutex_t *bm)
{
    uint32_t w;
    if (!bm)
        return NULL;
    w = (uint32_t)bm->v[0];
    if (w & HANDLE_TAG)
        return handle_get(w);

    mutexLock(&g_table_lock);
    w = (uint32_t)bm->v[0];
    if (!(w & HANDLE_TAG)) {
        NxMutex *nm = mutex_new(w == INIT_RECURSIVE || w == INIT_ERRORCHECK);
        uint32_t h = nm ? handle_add_locked(nm) : 0;
        if (!h) {
            mutexUnlock(&g_table_lock);
            free(nm);
            LOGE("pthread: mutex table exhausted");
            return NULL;
        }
        bm->v[0] = (int32_t)h;
        w = h;
    }
    mutexUnlock(&g_table_lock);
    return handle_get(w);
}

int bx_pthread_mutexattr_init(bionic_mutexattr_t *a)    { if (a) *a = BIONIC_MUTEX_NORMAL; return 0; }
int bx_pthread_mutexattr_destroy(bionic_mutexattr_t *a) { (void)a; return 0; }

int bx_pthread_mutexattr_settype(bionic_mutexattr_t *a, int type)
{
    if (!a || type < 0 || type > 2)
        return LX_EINVAL;
    *a = type;
    return 0;
}

int bx_pthread_mutex_init(bionic_mutex_t *bm, const bionic_mutexattr_t *a)
{
    NxMutex *nm;
    uint32_t h;
    if (!bm)
        return LX_EINVAL;
    nm = mutex_new(a && *a != BIONIC_MUTEX_NORMAL);
    if (!nm)
        return LX_ENOMEM;
    mutexLock(&g_table_lock);
    h = handle_add_locked(nm);
    mutexUnlock(&g_table_lock);
    if (!h) {
        free(nm);
        return LX_ENOMEM;
    }
    memset(bm, 0, sizeof(*bm));
    bm->v[0] = (int32_t)h;
    return 0;
}

int bx_pthread_mutex_destroy(bionic_mutex_t *bm)
{
    uint32_t w;
    if (!bm)
        return LX_EINVAL;
    mutexLock(&g_table_lock);
    w = (uint32_t)bm->v[0];
    if (w & HANDLE_TAG) {
        NxMutex *nm = handle_get(w);
        handle_remove_locked(w);
        free(nm);
    }
    bm->v[0] = 0;
    mutexUnlock(&g_table_lock);
    return 0;
}

int bx_pthread_mutex_lock(bionic_mutex_t *bm)
{
    NxMutex *nm = mutex_get(bm);
    if (!nm)
        return LX_EINVAL;
    if (nm->recursive)
        rmutexLock(&nm->rm);
    else
        mutexLock(&nm->m);
    return 0;
}

int bx_pthread_mutex_trylock(bionic_mutex_t *bm)
{
    NxMutex *nm = mutex_get(bm);
    if (!nm)
        return LX_EINVAL;
    if (nm->recursive)
        return rmutexTryLock(&nm->rm) ? 0 : LX_EBUSY;
    return mutexTryLock(&nm->m) ? 0 : LX_EBUSY;
}

int bx_pthread_mutex_unlock(bionic_mutex_t *bm)
{
    NxMutex *nm = mutex_get(bm);
    if (!nm)
        return LX_EINVAL;
    if (nm->recursive)
        rmutexUnlock(&nm->rm);
    else
        mutexUnlock(&nm->m);
    return 0;
}

/* -------------------------------------------------------------- conds -- */

typedef struct { CondVar cv; } NxCond;

static NxCond *cond_get(bionic_cond_t *bc)
{
    uint32_t w;
    if (!bc)
        return NULL;
    w = (uint32_t)bc->v[0];
    if (w & HANDLE_TAG)
        return handle_get(w);

    mutexLock(&g_table_lock);
    w = (uint32_t)bc->v[0];
    if (!(w & HANDLE_TAG)) {
        NxCond *nc = calloc(1, sizeof(*nc));
        uint32_t h = nc ? handle_add_locked(nc) : 0;
        if (!h) {
            mutexUnlock(&g_table_lock);
            free(nc);
            LOGE("pthread: cond table exhausted");
            return NULL;
        }
        condvarInit(&nc->cv);
        bc->v[0] = (int32_t)h;
        w = h;
    }
    mutexUnlock(&g_table_lock);
    return handle_get(w);
}

int bx_pthread_cond_init(bionic_cond_t *bc, const bionic_condattr_t *a)
{
    (void)a;
    if (!bc)
        return LX_EINVAL;
    memset(bc, 0, sizeof(*bc));
    return cond_get(bc) ? 0 : LX_ENOMEM;
}

int bx_pthread_cond_destroy(bionic_cond_t *bc)
{
    uint32_t w;
    if (!bc)
        return LX_EINVAL;
    mutexLock(&g_table_lock);
    w = (uint32_t)bc->v[0];
    if (w & HANDLE_TAG) {
        NxCond *nc = handle_get(w);
        handle_remove_locked(w);
        free(nc);
    }
    bc->v[0] = 0;
    mutexUnlock(&g_table_lock);
    return 0;
}

int bx_pthread_cond_signal(bionic_cond_t *bc)
{
    NxCond *nc = cond_get(bc);
    if (!nc)
        return LX_EINVAL;
    condvarWakeOne(&nc->cv);
    return 0;
}

int bx_pthread_cond_broadcast(bionic_cond_t *bc)
{
    NxCond *nc = cond_get(bc);
    if (!nc)
        return LX_EINVAL;
    condvarWakeAll(&nc->cv);
    return 0;
}

/* Returns 1 on timeout. */
static int cond_wait_ns(NxCond *nc, NxMutex *nm, u64 ns)
{
    Result rc;
    if (nm->recursive) {
        /* Fully release a recursive lock for the wait, then restore depth.
         * libnx tracks RMutex ownership in the inner Mutex word, so waiting on
         * that word is exactly a release-and-reacquire of the whole lock. */
        u32 depth = nm->rm.counter;
        nm->rm.counter = 0;
        rc = condvarWaitTimeout(&nc->cv, &nm->rm.lock, ns);
        nm->rm.counter = depth;
    } else {
        rc = condvarWaitTimeout(&nc->cv, &nm->m, ns);
    }
    return R_VALUE(rc) == KERNELRESULT(TimedOut);
}

int bx_pthread_cond_wait(bionic_cond_t *bc, bionic_mutex_t *bm)
{
    NxCond *nc = cond_get(bc);
    NxMutex *nm = mutex_get(bm);
    if (!nc || !nm)
        return LX_EINVAL;
    cond_wait_ns(nc, nm, COND_WAIT_CAP_NS);
    return 0;
}

static s64 realtime_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (s64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return (s64)armTicksToNs(armGetSystemTick());
}

int bx_pthread_cond_timedwait(bionic_cond_t *bc, bionic_mutex_t *bm,
                              const struct bionic_timespec *abstime)
{
    NxCond *nc = cond_get(bc);
    NxMutex *nm = mutex_get(bm);
    s64 rel;
    if (!nc || !nm || !abstime)
        return LX_EINVAL;
    rel = (s64)abstime->tv_sec * 1000000000LL + abstime->tv_nsec - realtime_ns();
    if (rel <= 0)
        return LX_ETIMEDOUT;
    if ((u64)rel > COND_WAIT_CAP_NS * 8) {
        /* A long timed wait: wait in slices so a lost wakeup costs a slice. */
        cond_wait_ns(nc, nm, COND_WAIT_CAP_NS * 8);
        return (s64)abstime->tv_sec * 1000000000LL + abstime->tv_nsec - realtime_ns() <= 0
                   ? LX_ETIMEDOUT : 0;
    }
    return cond_wait_ns(nc, nm, (u64)rel) ? LX_ETIMEDOUT : 0;
}

/* --------------------------------------------------------------- once -- */

static Mutex   g_once_lock;
static CondVar g_once_cv;

int bx_pthread_once(bionic_once_t *once, void (*fn)(void))
{
    if (!once || !fn)
        return LX_EINVAL;
    if (*once == 2)
        return 0;
    mutexLock(&g_once_lock);
    while (*once == 1)
        condvarWaitTimeout(&g_once_cv, &g_once_lock, COND_WAIT_CAP_NS);
    if (*once == 2) {
        mutexUnlock(&g_once_lock);
        return 0;
    }
    *once = 1;
    mutexUnlock(&g_once_lock);

    fn();

    mutexLock(&g_once_lock);
    *once = 2;
    condvarWakeAll(&g_once_cv);
    mutexUnlock(&g_once_lock);
    return 0;
}

/* --------------------------------------------------------------- keys -- */
/* bionic promises PTHREAD_KEYS_MAX = 128. newlib on Switch offers far fewer
 * native TLS slots, so keys are multiplexed onto one thread-local array. */

#define MAX_KEYS 128

static struct { int used; void (*dtor)(void *); } g_keys[MAX_KEYS];
static Mutex g_keys_lock;
static __thread void *t_key_values[MAX_KEYS];

int bx_pthread_key_create(bionic_key_t *key, void (*dtor)(void *))
{
    int i;
    if (!key)
        return LX_EINVAL;
    mutexLock(&g_keys_lock);
    for (i = 0; i < MAX_KEYS; i++) {
        if (!g_keys[i].used) {
            g_keys[i].used = 1;
            g_keys[i].dtor = dtor;
            *key = i;
            mutexUnlock(&g_keys_lock);
            return 0;
        }
    }
    mutexUnlock(&g_keys_lock);
    LOGE("pthread: out of keys");
    return LX_EAGAIN;
}

int bx_pthread_key_delete(bionic_key_t key)
{
    if (key < 0 || key >= MAX_KEYS)
        return LX_EINVAL;
    mutexLock(&g_keys_lock);
    g_keys[key].used = 0;
    g_keys[key].dtor = NULL;
    mutexUnlock(&g_keys_lock);
    return 0;
}

void *bx_pthread_getspecific(bionic_key_t key)
{
    if (key < 0 || key >= MAX_KEYS)
        return NULL;
    return t_key_values[key];
}

int bx_pthread_setspecific(bionic_key_t key, const void *value)
{
    if (key < 0 || key >= MAX_KEYS)
        return LX_EINVAL;
    t_key_values[key] = (void *)value;
    return 0;
}

static void run_key_destructors(void)
{
    int pass, i, again;
    for (pass = 0; pass < 4; pass++) {
        again = 0;
        for (i = 0; i < MAX_KEYS; i++) {
            void *v = t_key_values[i];
            void (*d)(void *) = g_keys[i].used ? g_keys[i].dtor : NULL;
            if (v && d) {
                t_key_values[i] = NULL;
                d(v);
                again = 1;
            }
        }
        if (!again)
            break;
    }
}

/* ------------------------------------------------------------ threads -- */

typedef struct {
    void *(*fn)(void *);
    void *arg;
} Trampoline;

static void *thread_trampoline(void *p)
{
    Trampoline t = *(Trampoline *)p;
    void *ret;
    free(p);
    ret = t.fn(t.arg);
    run_key_destructors();
    return ret;
}

int bx_pthread_attr_init(bionic_attr_t *a)
{
    if (!a)
        return LX_EINVAL;
    memset(a, 0, sizeof(*a));
    a->stack_size = 1024 * 1024;
    a->guard_size = 4096;
    return 0;
}

int bx_pthread_attr_destroy(bionic_attr_t *a) { (void)a; return 0; }

int bx_pthread_attr_setdetachstate(bionic_attr_t *a, int state)
{
    if (!a)
        return LX_EINVAL;
    if (state)
        a->flags |= BIONIC_ATTR_FLAG_DETACHED;
    else
        a->flags &= ~BIONIC_ATTR_FLAG_DETACHED;
    return 0;
}

int bx_pthread_attr_setstacksize(bionic_attr_t *a, size_t size)
{
    if (!a || size < 16384)
        return LX_EINVAL;
    a->stack_size = size;
    return 0;
}

int bx_pthread_create(bionic_pthread_t *out, const bionic_attr_t *a,
                      void *(*fn)(void *), void *arg)
{
    pthread_attr_t na;
    pthread_t nt;
    Trampoline *t;
    size_t stack = a ? a->stack_size : 1024 * 1024;
    int rc;

    if (!out || !fn)
        return LX_EINVAL;
    if (stack < 256 * 1024)
        stack = 256 * 1024;
    stack = (stack + 0xFFF) & ~(size_t)0xFFF;

    t = malloc(sizeof(*t));
    if (!t)
        return LX_EAGAIN;
    t->fn = fn;
    t->arg = arg;

    pthread_attr_init(&na);
    pthread_attr_setstacksize(&na, stack);
    rc = pthread_create(&nt, &na, thread_trampoline, t);
    pthread_attr_destroy(&na);
    if (rc != 0) {
        free(t);
        LOGE("pthread_create failed (%d), stack %zu KB", rc, stack / 1024);
        return LX_EAGAIN;
    }
    if (a && (a->flags & BIONIC_ATTR_FLAG_DETACHED))
        pthread_detach(nt);
    *out = (bionic_pthread_t)nt;
    LOGD("pthread_create: %p (stack %zu KB)", (void *)fn, stack / 1024);
    return 0;
}

int bx_pthread_join(bionic_pthread_t t, void **ret)
{
    int rc = pthread_join((pthread_t)t, ret);
    return rc ? bx_errno_to_linux(rc) : 0;
}

int bx_pthread_detach(bionic_pthread_t t)
{
    int rc = pthread_detach((pthread_t)t);
    return rc ? bx_errno_to_linux(rc) : 0;
}

bionic_pthread_t bx_pthread_self(void) { return (bionic_pthread_t)pthread_self(); }
int bx_pthread_equal(bionic_pthread_t a, bionic_pthread_t b) { return a == b; }

void bx_pthread_exit(void *ret)
{
    run_key_destructors();
    pthread_exit(ret);
}

int bx_pthread_getschedparam(bionic_pthread_t t, int *policy, struct bionic_sched_param *p)
{
    (void)t;
    if (policy)
        *policy = 0;
    if (p)
        p->sched_priority = 0;
    return 0;
}

int bx_pthread_setschedparam(bionic_pthread_t t, int policy, const struct bionic_sched_param *p)
{
    (void)t; (void)policy; (void)p;
    return 0;
}

int bx_sched_get_priority_max(int policy) { return policy == 0 ? 0 : 99; }
int bx_sched_get_priority_min(int policy) { return policy == 0 ? 0 : 1; }

int bx_sched_yield(void)
{
    svcSleepThread(-2);      /* yield to any thread, including other cores */
    return 0;
}

/* --------------------------------------------------------- semaphores -- */

typedef struct {
    Mutex   lock;
    CondVar cv;
    int     count;
} NxSem;

static NxSem *sem_get(bionic_sem_t *s)
{
    uint32_t w;
    if (!s)
        return NULL;
    w = s->v[0];
    if (w & HANDLE_TAG)
        return handle_get(w);
    mutexLock(&g_table_lock);
    w = s->v[0];
    if (!(w & HANDLE_TAG)) {
        NxSem *ns = calloc(1, sizeof(*ns));
        uint32_t h = ns ? handle_add_locked(ns) : 0;
        if (!h) {
            mutexUnlock(&g_table_lock);
            free(ns);
            return NULL;
        }
        s->v[0] = h;
        w = h;
    }
    mutexUnlock(&g_table_lock);
    return handle_get(w);
}

int bx_sem_init(bionic_sem_t *s, int shared, unsigned value)
{
    NxSem *ns;
    (void)shared;
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    ns = sem_get(s);
    if (!ns) {
        errno = LX_ENOMEM;
        return -1;
    }
    ns->count = (int)value;
    return 0;
}

int bx_sem_destroy(bionic_sem_t *s)
{
    uint32_t w;
    if (!s)
        return -1;
    mutexLock(&g_table_lock);
    w = s->v[0];
    if (w & HANDLE_TAG) {
        NxSem *ns = handle_get(w);
        handle_remove_locked(w);
        free(ns);
    }
    s->v[0] = 0;
    mutexUnlock(&g_table_lock);
    return 0;
}

int bx_sem_post(bionic_sem_t *s)
{
    NxSem *ns = sem_get(s);
    if (!ns)
        return -1;
    mutexLock(&ns->lock);
    ns->count++;
    condvarWakeOne(&ns->cv);
    mutexUnlock(&ns->lock);
    return 0;
}

int bx_sem_wait(bionic_sem_t *s)
{
    NxSem *ns = sem_get(s);
    if (!ns)
        return -1;
    mutexLock(&ns->lock);
    while (ns->count <= 0)
        condvarWaitTimeout(&ns->cv, &ns->lock, COND_WAIT_CAP_NS);
    ns->count--;
    mutexUnlock(&ns->lock);
    return 0;
}

int bx_sem_trywait(bionic_sem_t *s)
{
    NxSem *ns = sem_get(s);
    int ok;
    if (!ns)
        return -1;
    mutexLock(&ns->lock);
    ok = ns->count > 0;
    if (ok)
        ns->count--;
    mutexUnlock(&ns->lock);
    if (!ok) {
        errno = LX_EAGAIN;
        return -1;
    }
    return 0;
}

int bx_sem_getvalue(bionic_sem_t *s, int *value)
{
    NxSem *ns = sem_get(s);
    if (!ns || !value)
        return -1;
    mutexLock(&ns->lock);
    *value = ns->count;
    mutexUnlock(&ns->lock);
    return 0;
}

/* -------------------------------------------------------------- futex -- */
/* Both modules call syscall() for exactly one thing: futex (98), from the
 * C++ runtime's once/guard machinery. WAIT and WAKE are enough. */

#define LX_SYS_FUTEX       98
#define LX_FUTEX_WAIT      0
#define LX_FUTEX_WAKE      1
#define LX_FUTEX_CMD_MASK  0x7F
#define FUTEX_BUCKETS      64

static struct { Mutex lock; CondVar cv; } g_futex[FUTEX_BUCKETS];

long bx_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a5; (void)a6;
    if (n == LX_SYS_FUTEX) {
        int *addr = (int *)a1;
        int op = (int)a2 & LX_FUTEX_CMD_MASK;
        unsigned b = (unsigned)(((uintptr_t)addr >> 2) % FUTEX_BUCKETS);
        if (op == LX_FUTEX_WAIT) {
            const struct bionic_timespec *ts = (const struct bionic_timespec *)a4;
            u64 ns = COND_WAIT_CAP_NS;
            int timed_out = 0;
            if (ts) {
                s64 req = (s64)ts->tv_sec * 1000000000LL + ts->tv_nsec;
                if (req <= 0) {
                    errno = LX_ETIMEDOUT;
                    return -1;
                }
                if ((u64)req < ns)
                    ns = (u64)req;
            }
            mutexLock(&g_futex[b].lock);
            if (*addr != (int)a3) {
                mutexUnlock(&g_futex[b].lock);
                errno = LX_EAGAIN;
                return -1;
            }
            timed_out = R_VALUE(condvarWaitTimeout(&g_futex[b].cv, &g_futex[b].lock, ns)) ==
                        KERNELRESULT(TimedOut);
            mutexUnlock(&g_futex[b].lock);
            if (timed_out && ts && (u64)((s64)ts->tv_sec * 1000000000LL + ts->tv_nsec) <= ns) {
                errno = LX_ETIMEDOUT;
                return -1;
            }
            return 0;                        /* woken, or a legal spurious wakeup */
        }
        if (op == LX_FUTEX_WAKE) {
            mutexLock(&g_futex[b].lock);
            condvarWakeAll(&g_futex[b].cv);
            mutexUnlock(&g_futex[b].lock);
            return a3 > 0 ? a3 : 0;
        }
        LOG_ONCE("syscall: futex op %d not implemented", op);
        errno = LX_ENOSYS;
        return -1;
    }
    LOGE("syscall(%ld) not implemented", n);
    errno = LX_ENOSYS;
    return -1;
}
