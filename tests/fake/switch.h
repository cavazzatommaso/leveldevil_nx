/* Host stand-ins for the libnx calls savecache.c uses (tests only). */
#include <pthread.h>
#include <stdint.h>
#include <time.h>
typedef uint64_t u64; typedef uint32_t Result;
/* libnx locks are valid when zeroed; macOS pthread ones are not, so these
 * initialise themselves on first use (the first use is single-threaded). */
typedef struct { int init; pthread_mutex_t m; } Mutex;
typedef struct { int init; pthread_cond_t c; } CondVar;
static inline pthread_mutex_t *mx(Mutex *m) { if (!m->init) { pthread_mutex_init(&m->m, 0); m->init = 1; } return &m->m; }
static inline pthread_cond_t *cv(CondVar *c) { if (!c->init) { pthread_cond_init(&c->c, 0); c->init = 1; } return &c->c; }
typedef struct { pthread_t t; void (*fn)(void *); void *arg; } Thread;
#define R_FAILED(r) ((r) != 0)
static inline void mutexLock(Mutex *m) { pthread_mutex_lock(mx(m)); }
static inline void mutexUnlock(Mutex *m) { pthread_mutex_unlock(mx(m)); }
static inline void condvarWait(CondVar *c, Mutex *m) { pthread_cond_wait(cv(c), mx(m)); }
static inline void condvarWakeAll(CondVar *c) { pthread_cond_broadcast(cv(c)); }
static inline u64 armGetSystemTick(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static inline u64 armNsToTicks(u64 ns) { return ns; }
static inline u64 armTicksToNs(u64 t) { return t; }
static inline void condvarWaitTimeout(CondVar *c, Mutex *m, u64 ns) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_nsec += (long)(ns % 1000000000ull); t.tv_sec += (time_t)(ns / 1000000000ull) + t.tv_nsec / 1000000000L; t.tv_nsec %= 1000000000L; pthread_cond_timedwait(cv(c), mx(m), &t); }
static void *fake_entry(void *a) { Thread *t = a; t->fn(t->arg); return 0; }
static inline Result threadCreate(Thread *t, void (*fn)(void *), void *arg, void *s, unsigned long sz, int prio, int core) { (void)s; (void)sz; (void)prio; (void)core; t->fn = fn; t->arg = arg; return 0; }
static inline Result threadStart(Thread *t) { return (Result)pthread_create(&t->t, 0, fake_entry, t); }
