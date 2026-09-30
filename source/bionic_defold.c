/* bionic_defold.c -- the libc imports the Defold engine adds on top of what
 * the Stencyl ports needed.
 *
 * Mostly fortify wrappers (__memcpy_chk and friends: the size check is the
 * compiler's, the work is newlib's) and small functions whose bionic and
 * newlib ABIs differ only in a struct or a FILE*. Networking stays offline,
 * processes cannot be spawned, and timers do not exist; each of those says so
 * the way bionic does when a call fails.
 *
 * MIT licensed, see LICENSE.
 */
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>

#include "bionic.h"
#include "log.h"
#include "so_util.h"

/* ---- fortify: the check is the caller's, the work is newlib's ---- */
void  *bx_memcpy_chk(void *d, const void *s, size_t n, size_t dl)  { (void)dl; return memcpy(d, s, n); }
void  *bx_memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
void  *bx_memset_chk(void *d, int c, size_t n, size_t dl)          { (void)dl; return memset(d, c, n); }
size_t bx_strlen_chk(const char *s, size_t sl)                     { (void)sl; return strlen(s); }
char  *bx_strchr_chk(const char *s, int c, size_t sl)              { (void)sl; return strchr(s, c); }
char  *bx_strrchr_chk(const char *s, int c, size_t sl)             { (void)sl; return strrchr(s, c); }
long   bx_read_chk(int fd, void *buf, size_t n, size_t bl)         { (void)bl; return bx_read(fd, buf, n); }
int    bx_open_2(const char *path, int flags)                      { return bx_open(path, flags, 0); }

int bx_vsnprintf_chk(char *buf, size_t n, int flags, size_t bl, const char *fmt, va_list ap)
{
    (void)flags; (void)bl;
    return vsnprintf(buf, n, fmt, ap);
}

int bx_vsprintf_chk(char *buf, int flags, size_t bl, const char *fmt, va_list ap)
{
    (void)flags;
    return vsnprintf(buf, bl, fmt, ap);
}

void bx_FD_CLR_chk(int fd, void *set, size_t setsize)
{
    if (fd >= 0 && (size_t)fd < setsize * 8)
        ((unsigned long *)set)[fd / 64] &= ~(1UL << (fd % 64));
}

/* ---- ctype: bionic declares `extern const char *_ctype_` -- a POINTER to
 * the BSD table, read as (_ctype_ + 1)[c]. The import is that pointer. ---- */
char bx_ctype_[1 + 256];
const char *bx_ctype_ptr = bx_ctype_;

static void ctype_init(void)
{
    int c;
    for (c = 0; c < 256; c++) {
        char v = 0;
        if (c < 128) {
            if (isupper(c))  v |= 0x01;
            if (islower(c))  v |= 0x02;
            if (isdigit(c))  v |= 0x04;
            if (isspace(c))  v |= 0x08;
            if (ispunct(c))  v |= 0x10;
            if (iscntrl(c))  v |= 0x20;
            if (isxdigit(c) && !isdigit(c)) v |= 0x40;
            if (c == ' ')    v |= (char)0x80;
        }
        bx_ctype_[1 + c] = v;
    }
}

/* ---- stdio taking a FILE* ---- */
int bx_getc(FILE *f) { return bx_fgetc(f); }

int bx_ungetc(int c, FILE *f)
{
    return f ? ungetc(c, bx_real_file(f)) : EOF;
}

int bx_fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (!f)
        return EOF;
    va_start(ap, fmt);
    n = vfscanf(bx_real_file(f), fmt, ap);
    va_end(ap);
    return n;
}

int bx_vprintf(const char *fmt, va_list ap)
{
    return bx_vfprintf((FILE *)(bx_sF + BIONIC_FILE_SIZE), fmt, ap);
}

void *bx_popen(const char *cmd, const char *mode)
{
    (void)mode;
    LOGI("popen(%s): no processes on this port", cmd ? cmd : "");
    errno = LX_ENOSYS;
    return NULL;
}

int bx_pclose(void *f) { (void)f; return -1; }

int bx_mkstemp(char *tmpl)
{
    int fd = mkstemp(tmpl);
    if (fd < 0)
        bx_fix_errno();
    return fd;
}

/* ---- time ---- */
struct bionic_tm *bx_localtime(const long *t)
{
    static __thread struct bionic_tm tm;
    return bx_localtime_r(t, &tm);
}

long bx_clock(void)
{
    /* CLOCKS_PER_SEC is 1000000 in bionic */
    return (long)(armTicksToNs(armGetSystemTick()) / 1000);
}

int bx_usleep(unsigned us)
{
    svcSleepThread((s64)us * 1000);
    return 0;
}

int bx_setitimer(int which, const void *nv, void *ov)
{
    (void)which; (void)nv; (void)ov;
    errno = LX_ENOSYS;
    return -1;
}

void *bx_mremap(void *old, size_t oldsz, size_t newsz, int flags, ...)
{
    (void)old; (void)oldsz; (void)newsz; (void)flags;
    errno = LX_ENOMEM;
    return (void *)-1;
}

/* ---- threads ---- */
int bx_pthread_attr_getstacksize(const bionic_attr_t *a, size_t *size)
{
    if (size)
        *size = a ? a->stack_size : 1024 * 1024;
    return 0;
}

/* Only ever used to learn the current thread's stack bounds. libnx knows
 * them exactly for threads it created; for the main thread, the loader's
 * reported range is used. */
int bx_pthread_getattr_np(bionic_pthread_t t, bionic_attr_t *a)
{
    Thread *th = threadGetSelf();
    (void)t;
    if (!a)
        return LX_EINVAL;
    memset(a, 0, sizeof(*a));
    if (th && th->stack_mirror && th->stack_sz) {
        a->stack_base = th->stack_mirror;
        a->stack_size = th->stack_sz;
    } else {
        uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
        a->stack_size = 1024 * 1024;
        a->stack_base = (void *)((sp & ~(uintptr_t)0xFFF) - a->stack_size + 0x1000);
    }
    a->guard_size = 0x1000;
    return 0;
}

/* Defold names its threads from inside them (dmThread::SetThreadName on
 * pthread_self), which is the one moment the port can tell the engine thread
 * from the rest. Every thread starts on the process's default core, so
 * without this the engine shares core 0 with the looper, sound, http and job
 * threads while cores 1 and 2 sit idle. */
static void place_thread(const char *name)
{
    int core = -1;
    u64 mask = 0;

    if (!strcmp(name, "engine_main"))
        core = 1;
    else if (!strcmp(name, "sound"))
        core = 2;
    if (core < 0)
        return;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !(mask & (1ULL << core))) {
        LOGI("thread: core %d not available for %s (mask 0x%lx)", core, name, (unsigned long)mask);
        return;
    }
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1U << core)))
        LOGI("thread: could not move %s to core %d", name, core);
    else
        LOGI("thread: %s pinned to core %d", name, core);
}

int bx_pthread_setname_np(bionic_pthread_t t, const char *name)
{
    LOGD("thread name: %s", name ? name : "");
    if (name && t == bx_pthread_self())
        place_thread(name);
    return 0;
}

/* ---- networking: offline ---- */
static __thread int t_h_errno;
int *bx_get_h_errno(void) { return &t_h_errno; }
void *bx_gethostbyname(const char *name) { (void)name; t_h_errno = 1; return NULL; }   /* HOST_NOT_FOUND */
const char *bx_hstrerror(int e) { (void)e; return "network unavailable"; }
int bx_inet_aton(const char *cp, void *out)
{
    unsigned a = bx_inet_addr(cp);
    if (a == 0xFFFFFFFFu && strcmp(cp, "255.255.255.255"))
        return 0;
    if (out)
        memcpy(out, &a, 4);
    return 1;
}
int bx_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hl, char *serv, unsigned sl, int flags)
{
    (void)sa; (void)salen; (void)flags;
    if (host && hl) host[0] = '\0';
    if (serv && sl) serv[0] = '\0';
    return -2;                                  /* EAI_NONAME */
}

/* ---- dladdr: which loaded module holds this address ---- */
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } bionic_dl_info;

int bx_dladdr(const void *addr, void *vinfo)
{
    bionic_dl_info *info = vinfo;
    so_module *m = so_module_by_addr((uintptr_t)addr);
    const char *mod = NULL, *sym = NULL;
    uintptr_t off = 0;
    if (!m || !info)
        return 0;
    memset(info, 0, sizeof(*info));
    info->dli_fname = m->name;
    info->dli_fbase = m->load_virtbase;
    if (so_symbolize((uintptr_t)addr, &mod, &sym, &off) && sym) {
        info->dli_sname = sym;
        info->dli_saddr = (void *)((uintptr_t)addr - off);
    }
    return 1;
}

/* ---- GLES: OES_vertex_array_object is core in GLES 3, which mesa has ---- */
extern void glBindVertexArray(unsigned array);
extern void glGenVertexArrays(int n, unsigned *arrays);
void bx_glBindVertexArrayOES(unsigned array)       { glBindVertexArray(array); }
void bx_glGenVertexArraysOES(int n, unsigned *a)   { glGenVertexArrays(n, a); }

void bx_defold_init(void)
{
    ctype_init();
}
