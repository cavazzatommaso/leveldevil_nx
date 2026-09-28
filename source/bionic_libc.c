/* bionic_libc.c -- environment, time, system queries, logging, lifecycle.
 * MIT licensed, see LICENSE.
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

#include "app.h"
#include "bionic.h"
#include "config.h"
#include "dl_bridge.h"
#include "log.h"
#include "paths.h"
#include "so_util.h"

/* ---------------------------------------------------------------- errno -- */

int bx_errno_to_linux(int e)
{
    /* if-chain rather than switch: several of these share a value on some
     * C libraries and a duplicate case label would not compile there */
    if (e == ENAMETOOLONG) return LX_ENAMETOOLONG;
    if (e == ENOTEMPTY)    return LX_ENOTEMPTY;
    if (e == ELOOP)        return LX_ELOOP;
    if (e == ENOSYS)       return LX_ENOSYS;
    if (e == ETIMEDOUT)    return LX_ETIMEDOUT;
    if (e == EDEADLK)      return LX_EDEADLK;
    if (e == EOVERFLOW)    return LX_EOVERFLOW;
    if (e == ENOTSUP)      return LX_EOPNOTSUPP;
    if (e == EOPNOTSUPP)   return LX_EOPNOTSUPP;
    if (e == EAFNOSUPPORT) return LX_EAFNOSUPPORT;
    if (e == ENETDOWN)     return LX_ENETDOWN;
    if (e == ENETUNREACH)  return LX_ENETUNREACH;
    if (e == ECONNREFUSED) return LX_ECONNREFUSED;
    return e;                 /* 1..34 agree */
}

void bx_fix_errno(void) { errno = bx_errno_to_linux(errno); }

/* ---------------------------------------------------------- environment -- */

char **bx_environ;
static int    g_env_count, g_env_cap;
static RMutex g_env_lock;
static u64    g_tick0;

static int env_find(const char *name, size_t nlen)
{
    int i;
    for (i = 0; i < g_env_count; i++)
        if (!strncmp(bx_environ[i], name, nlen) && bx_environ[i][nlen] == '=')
            return i;
    return -1;
}

int bx_setenv(const char *name, const char *value, int overwrite)
{
    size_t nlen;
    int idx;
    char *entry;

    if (!name || !*name || strchr(name, '=')) {
        errno = LX_EINVAL;
        return -1;
    }
    if (!value)
        value = "";
    nlen = strlen(name);
    rmutexLock(&g_env_lock);
    idx = env_find(name, nlen);
    if (idx >= 0 && !overwrite) {
        rmutexUnlock(&g_env_lock);
        return 0;
    }
    entry = malloc(nlen + strlen(value) + 2);
    if (!entry) {
        rmutexUnlock(&g_env_lock);
        errno = LX_ENOMEM;
        return -1;
    }
    sprintf(entry, "%s=%s", name, value);
    if (idx >= 0) {
        /* The old string is deliberately leaked: a pointer returned by an
         * earlier getenv() may still be in use. */
        bx_environ[idx] = entry;
    } else {
        if (g_env_count + 2 > g_env_cap) {
            int cap = g_env_cap ? g_env_cap * 2 : 32;
            char **n = malloc((size_t)cap * sizeof(char *));
            if (!n) {
                free(entry);
                rmutexUnlock(&g_env_lock);
                errno = LX_ENOMEM;
                return -1;
            }
            if (bx_environ)
                memcpy(n, bx_environ, (size_t)(g_env_count + 1) * sizeof(char *));
            /* old array leaked for the same reason as above */
            bx_environ = n;
            g_env_cap = cap;
        }
        bx_environ[g_env_count++] = entry;
        bx_environ[g_env_count] = NULL;
    }
    rmutexUnlock(&g_env_lock);
    return 0;
}

char *bx_getenv(const char *name)
{
    char *r = NULL;
    int idx;
    if (!name)
        return NULL;
    rmutexLock(&g_env_lock);
    idx = env_find(name, strlen(name));
    if (idx >= 0)
        r = bx_environ[idx] + strlen(name) + 1;
    rmutexUnlock(&g_env_lock);
    return r;
}

void bx_libc_init(void)
{
    g_tick0 = armGetSystemTick();
    bx_setenv("SDL_DYNAMIC_API", HS_SDL_SENTINEL, 1);
    /* liblime's OpenAL Soft has opensl, sdl2, null and wave backends; sdl2
     * sends the mix through switch-sdl2's audren driver. */
    bx_setenv("ALSOFT_DRIVERS", "sdl2", 1);
    bx_setenv("HOME", paths_save_nodev(), 1);
    bx_setenv("TMPDIR", paths_save_nodev(), 1);
    bx_setenv("LANG", "en_US.UTF-8", 1);
    if (log_get_level() >= HS_LOG_DEBUG)
        bx_setenv("ALSOFT_LOGLEVEL", "3", 1);

    /* hxcpp's collector reads these (getenv + atoi, in bytes). Its defaults --
     * 8 MB working memory, 4 MB minimum free -- are sized for phones and make
     * it collect often; on a console with gigabytes free, collecting less
     * often removes a recurring source of dropped frames. */
    if (g_cfg.gc_working_mb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", g_cfg.gc_working_mb * 1024 * 1024);
        bx_setenv("HXCPP_MINIMUM_WORKING_MEMORY", buf, 1);
    }
    if (g_cfg.gc_free_mb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", g_cfg.gc_free_mb * 1024 * 1024);
        bx_setenv("HXCPP_MINIMUM_FREE_SPACE", buf, 1);
    }
    LOGI("gc: working memory %d MB, minimum free %d MB%s", g_cfg.gc_working_mb,
         g_cfg.gc_free_mb, g_cfg.gc_working_mb ? "" : " (the game's own settings)");
}

/* ----------------------------------------------------------------- time -- */

int bx_clock_gettime(int clk, struct bionic_timespec *ts)
{
    if (!ts) {
        errno = LX_EINVAL;
        return -1;
    }
    /* REALTIME, REALTIME_COARSE, REALTIME_ALARM, TAI */
    if (clk == 0 || clk == 5 || clk == 8 || clk == 11) {
        struct timespec t;
        if (clock_gettime(CLOCK_REALTIME, &t) == 0) {
            ts->tv_sec = (long)t.tv_sec;
            ts->tv_nsec = t.tv_nsec;
            return 0;
        }
    }
    /* MONOTONIC, PROCESS/THREAD_CPUTIME, MONOTONIC_RAW/COARSE, BOOTTIME */
    {
        u64 ns = armTicksToNs(armGetSystemTick());
        ts->tv_sec = (long)(ns / 1000000000ULL);
        ts->tv_nsec = (long)(ns % 1000000000ULL);
    }
    return 0;
}

int bx_nanosleep(const struct bionic_timespec *req, struct bionic_timespec *rem)
{
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = LX_EINVAL;
        return -1;
    }
    svcSleepThread((s64)req->tv_sec * 1000000000LL + req->tv_nsec);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

static void tm_to_bionic(const struct tm *in, struct bionic_tm *out)
{
    memset(out, 0, sizeof(*out));
    out->tm_sec = in->tm_sec;
    out->tm_min = in->tm_min;
    out->tm_hour = in->tm_hour;
    out->tm_mday = in->tm_mday;
    out->tm_mon = in->tm_mon;
    out->tm_year = in->tm_year;
    out->tm_wday = in->tm_wday;
    out->tm_yday = in->tm_yday;
    out->tm_isdst = in->tm_isdst;
    out->tm_zone = "UTC";
}

static void tm_from_bionic(const struct bionic_tm *in, struct tm *out)
{
    memset(out, 0, sizeof(*out));
    out->tm_sec = in->tm_sec;
    out->tm_min = in->tm_min;
    out->tm_hour = in->tm_hour;
    out->tm_mday = in->tm_mday;
    out->tm_mon = in->tm_mon;
    out->tm_year = in->tm_year;
    out->tm_wday = in->tm_wday;
    out->tm_yday = in->tm_yday;
    out->tm_isdst = in->tm_isdst;
}

struct bionic_tm *bx_localtime_r(const long *t, struct bionic_tm *out)
{
    struct tm tmp;
    time_t tt;
    if (!t || !out)
        return NULL;
    tt = (time_t)*t;
    if (!localtime_r(&tt, &tmp))
        return NULL;
    tm_to_bionic(&tmp, out);
    return out;
}

struct bionic_tm *bx_gmtime_r(const long *t, struct bionic_tm *out)
{
    struct tm tmp;
    time_t tt;
    if (!t || !out)
        return NULL;
    tt = (time_t)*t;
    if (!gmtime_r(&tt, &tmp))
        return NULL;
    tm_to_bionic(&tmp, out);
    return out;
}

struct bionic_tm *bx_gmtime(const long *t)
{
    static __thread struct bionic_tm out;
    return bx_gmtime_r(t, &out);
}

long bx_mktime(struct bionic_tm *btm)
{
    struct tm tmp;
    time_t r;
    if (!btm)
        return -1;
    tm_from_bionic(btm, &tmp);
    r = mktime(&tmp);
    tm_to_bionic(&tmp, btm);
    return (long)r;
}

size_t bx_strftime(char *s, size_t max, const char *fmt, const struct bionic_tm *btm)
{
    struct tm tmp;
    if (!btm)
        return 0;
    tm_from_bionic(btm, &tmp);
    return strftime(s, max, fmt, &tmp);
}

long bx_times(struct bionic_tms *t)
{
    long ticks = (long)(armTicksToNs(armGetSystemTick() - g_tick0) / 10000000ULL);
    if (t) {
        t->tms_utime = ticks;
        t->tms_stime = 0;
        t->tms_cutime = 0;
        t->tms_cstime = 0;
    }
    return ticks;
}

/* ------------------------------------------------------------- queries -- */

long bx_sysconf(int name)
{
    u64 total = 0, used = 0;
    switch (name) {
    case 0x00: return 2097152;               /* _SC_ARG_MAX */
    case 0x06: return 100;                   /* _SC_CLK_TCK */
    case 0x0b: return 1024;                  /* _SC_OPEN_MAX */
    case 0x27:                               /* _SC_PAGESIZE */
    case 0x28: return 4096;                  /* _SC_PAGE_SIZE */
    case 0x60:                               /* _SC_NPROCESSORS_CONF */
    case 0x61: return 3;                     /* _SC_NPROCESSORS_ONLN: cores 0-2 */
    case 0x62:                               /* _SC_PHYS_PAGES */
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        return (long)(total / 4096);
    case 0x63:                               /* _SC_AVPHYS_PAGES */
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        return (long)((total > used ? total - used : 0) / 4096);
    default:
        LOGD("sysconf(0x%x) unsupported", name);
        errno = LX_EINVAL;
        return -1;
    }
}

int bx_system_property_get(const char *name, char *value)
{
    static const struct { const char *k, *v; } props[] = {
        { "ro.build.version.sdk",     "29" },
        { "ro.build.version.release", "10" },
        { "ro.product.model",         "Nintendo Switch" },
        { "ro.product.manufacturer",  "Nintendo" },
        { "ro.product.brand",         "Nintendo" },
        { "ro.product.device",        "switch" },
        { "ro.hardware",              "nx" },
        { "ro.product.cpu.abi",       "arm64-v8a" },
    };
    size_t i;
    if (!value)
        return 0;
    value[0] = '\0';
    if (!name)
        return 0;
    for (i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        if (!strcmp(name, props[i].k)) {
            snprintf(value, 92, "%s", props[i].v);   /* PROP_VALUE_MAX */
            return (int)strlen(value);
        }
    }
    return 0;
}

char *bx_setlocale(int cat, const char *locale)
{
    static char c_locale[] = "C";
    (void)cat; (void)locale;
    return c_locale;
}

int bx_strerror_r(int e, char *buf, size_t n)
{
    if (!buf || !n)
        return LX_ERANGE;
    snprintf(buf, n, "%s", strerror(e));
    return 0;
}

int bx_isfinitef(float f) { return isfinite(f); }

/* ------------------------------------------------------------ lifecycle -- */

uintptr_t bx_stack_chk_guard = 0x5a17c0de2bad7ea5ULL;

static void log_caller(const char *what, uintptr_t lr)
{
    const char *mod, *sym;
    uintptr_t off;
    if (so_symbolize(lr, &mod, &sym, &off))
        LOGE("%s called from %s!%s+0x%lx", what, mod, sym ? sym : "?", (unsigned long)off);
    else
        LOGE("%s called from %p", what, (void *)lr);
}

void bx_abort(void)
{
    log_caller("abort()", (uintptr_t)__builtin_return_address(0));
    log_flush();
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

void bx_exit(int code)
{
    log_caller("exit()", (uintptr_t)__builtin_return_address(0));
    LOGI("exit(%d) requested by the game", code);
    hs_request_exit(code);
    hs_park_forever();
}

void bx_stack_chk_fail(void)
{
    log_caller("__stack_chk_fail (stack corruption)", (uintptr_t)__builtin_return_address(0));
    log_flush();
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

int bx_cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    /* Module destructors must not run: exit is by process termination, and
     * destructors would race threads that are still alive. */
    (void)fn; (void)arg; (void)dso;
    return 0;
}

void bx_cxa_finalize(void *dso) { (void)dso; }
void bx_google_region(void) {}

/* -------------------------------------------------------------- logging -- */

static const char LOG_PRIO[] = "??VDIWEFS";

int bx_android_log_write(int prio, const char *tag, const char *text)
{
    if (prio <= 2 && log_get_level() < HS_LOG_DEBUG)
        return 0;
    log_printf("%c/%s: %s", LOG_PRIO[(unsigned)prio < 9 ? prio : 0],
               tag ? tag : "", text ? text : "");
    return 1;
}

int bx_android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (prio <= 2 && log_get_level() < HS_LOG_DEBUG)
        return 0;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    return bx_android_log_write(prio, tag, buf);
}

/* --------------------------------------------------- process and signals -- */

int bx_getpid(void) { return 1000; }
unsigned bx_geteuid(void) { return 10000; }

void *bx_getpwuid(unsigned uid)
{
    (void)uid;
    errno = LX_ENOENT;
    return NULL;
}

int bx_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old)
{
    (void)sig; (void)act;
    if (old)
        memset(old, 0, sizeof(*old));
    return 0;
}

void *bx_signal(int sig, void *handler)
{
    (void)sig; (void)handler;
    return NULL;                           /* SIG_DFL */
}

int bx_sigemptyset(unsigned long *set)
{
    if (set)
        *set = 0;
    return 0;
}

int bx_sigaddset(unsigned long *set, int sig)
{
    if (!set || sig <= 0 || sig > 64) {
        errno = LX_EINVAL;
        return -1;
    }
    *set |= 1UL << (sig - 1);
    return 0;
}

int bx_pthread_sigmask(int how, const unsigned long *set, unsigned long *old)
{
    (void)how; (void)set;
    if (old)
        *old = 0;
    return 0;
}

int bx_raise(int sig)
{
    log_caller("raise()", (uintptr_t)__builtin_return_address(0));
    LOGI("raise(%d)", sig);
    if (sig == 6)                           /* SIGABRT */
        bx_abort();
    return 0;
}

unsigned bx_alarm(unsigned s) { (void)s; return 0; }

int bx_system(const char *cmd)
{
    if (!cmd)
        return 0;                           /* no command processor */
    LOGI("system(\"%s\") ignored", cmd);
    return -1;
}

int bx_fork(void)
{
    LOGI("fork() not available");
    errno = LX_ENOSYS;
    return -1;
}

int bx_execvp(const char *file, char *const argv[])
{
    (void)argv;
    LOGI("execvp(%s) not available", file ? file : "");
    errno = LX_ENOSYS;
    return -1;
}

int bx_waitpid(int pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    errno = LX_ECHILD;
    return -1;
}
