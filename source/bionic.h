/* bionic.h -- the parts of Android's C library ABI the modules can see.
 *
 * Both modules were compiled against bionic (arm64). Where bionic and newlib
 * agree -- memcpy, strtod, sin, malloc, the printf formatting engine -- the
 * import resolves straight to newlib. Everything declared here is where they
 * disagree, and the shim in bionic_*.c translates:
 *
 *   struct layouts   stat, dirent, tm, pthread objects, sem_t, FILE (__sF)
 *   constants        open() flags, clock ids, sysconf names, errno values
 *   semantics        exit() from a non-main thread, futex, pthread keys
 *
 * Sizes are asserted, because a layout that is silently one field off is the
 * kind of bug that shows up three subsystems away.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef PB_BIONIC_H
#define PB_BIONIC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------------------------------------------------------- errno -- */
/* Values as the modules were compiled to expect them (Linux/bionic). The
 * classic 1..34 range is identical in newlib; these are the ones that are
 * not, plus the ones the shims set explicitly. */
#define LX_EPERM         1
#define LX_ENOENT        2
#define LX_ESRCH         3
#define LX_EINTR         4
#define LX_EBADF         9
#define LX_ECHILD       10
#define LX_EAGAIN       11
#define LX_ENOMEM       12
#define LX_EACCES       13
#define LX_EBUSY        16
#define LX_EEXIST       17
#define LX_ENOTDIR      20
#define LX_EINVAL       22
#define LX_EMFILE       24
#define LX_ENOTTY       25
#define LX_ERANGE       34
#define LX_EDEADLK      35
#define LX_ENAMETOOLONG 36
#define LX_ENOSYS       38
#define LX_ENOTEMPTY    39
#define LX_ELOOP        40
#define LX_EOVERFLOW    75
#define LX_EOPNOTSUPP   95
#define LX_EAFNOSUPPORT 97
#define LX_ENETDOWN    100
#define LX_ENETUNREACH 101
#define LX_ETIMEDOUT   110
#define LX_ECONNREFUSED 111

/* Convert an errno value produced by newlib into the Linux numbering. */
int bx_errno_to_linux(int e);
/* errno = bx_errno_to_linux(errno), for use right after a failed newlib call */
void bx_fix_errno(void);

/* ----------------------------------------------------------------- stdio -- */
#define BIONIC_FILE_SIZE 152
extern char bx_sF[3 * BIONIC_FILE_SIZE];     /* exported to the modules as __sF */
/* This NDK also exports stdin/stdout/stderr as FILE* variables. */
extern void *bx_stdin_var, *bx_stdout_var, *bx_stderr_var;

/* --------------------------------------------------------------- structs -- */
struct bionic_timespec { long tv_sec; long tv_nsec; };

struct bionic_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    struct bionic_timespec st_atim;
    struct bionic_timespec st_mtim;
    struct bionic_timespec st_ctim;
    uint32_t __unused4;
    uint32_t __unused5;
};

struct bionic_dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

struct bionic_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

struct bionic_tms { long tms_utime, tms_stime, tms_cutime, tms_cstime; };

/* Only ever zeroed by the shim; the modules never get a handler installed. */
struct bionic_sigaction { void *handler; unsigned long sa_mask; int sa_flags; void *sa_restorer; };

/* pthread objects. LP64 bionic makes the mutex/cond int32 arrays, i.e. 4-byte
 * aligned: the shims keep a table index in the first int32 and never do a
 * 64-bit load from these. */
typedef struct { int32_t v[10]; } bionic_mutex_t;
typedef struct { int32_t v[12]; } bionic_cond_t;
typedef struct { uint32_t v[4]; } bionic_sem_t;
typedef struct { int32_t  v[14]; } bionic_rwlock_t;   /* bionic LP64: 56 bytes */
typedef long bionic_mutexattr_t;
typedef long bionic_condattr_t;
typedef int  bionic_once_t;
typedef int  bionic_key_t;
typedef long bionic_pthread_t;
typedef struct {
    uint32_t flags;
    void    *stack_base;
    size_t   stack_size;
    size_t   guard_size;
    int32_t  sched_policy;
    int32_t  sched_priority;
    char     __reserved[16];
} bionic_attr_t;
struct bionic_sched_param { int sched_priority; };

#define BIONIC_ATTR_FLAG_DETACHED 1
#define BIONIC_MUTEX_NORMAL       0
#define BIONIC_MUTEX_RECURSIVE    1
#define BIONIC_MUTEX_ERRORCHECK   2

/* ------------------------------------------------------------- open(2) -- */
#define LX_O_ACCMODE   00000003
#define LX_O_WRONLY    00000001
#define LX_O_RDWR      00000002
#define LX_O_CREAT     00000100
#define LX_O_EXCL      00000200
#define LX_O_TRUNC     00001000
#define LX_O_APPEND    00002000
#define LX_O_NONBLOCK  00004000
#define LX_O_DIRECTORY 00040000

/* ------------------------------------------------------------ prototypes -- */
/* bionic_stdio.c */
FILE  *bx_real_file(FILE *f);
FILE  *bx_fopen(const char *path, const char *mode);
FILE  *bx_fdopen(int fd, const char *mode);
int    bx_fclose(FILE *f);
size_t bx_fread(void *p, size_t sz, size_t n, FILE *f);
size_t bx_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    bx_fseek(FILE *f, long off, int whence);
long   bx_ftell(FILE *f);
int    bx_fflush(FILE *f);
int    bx_feof(FILE *f);
int    bx_ferror(FILE *f);
void   bx_clearerr(FILE *f);
int    bx_fileno(FILE *f);
char  *bx_fgets(char *s, int n, FILE *f);
int    bx_fgetc(FILE *f);
int    bx_fputc(int c, FILE *f);
int    bx_fputs(const char *s, FILE *f);
int    bx_fprintf(FILE *f, const char *fmt, ...);
int    bx_vfprintf(FILE *f, const char *fmt, va_list ap);
int    bx_printf(const char *fmt, ...);
int    bx_puts(const char *s);
int    bx_putchar(int c);
int    bx_getchar(void);
void   bx_setbuf(FILE *f, char *buf);
int    bx_open(const char *path, int flags, int mode);
int    bx_close(int fd);
long   bx_read(int fd, void *buf, size_t n);
long   bx_write(int fd, const void *buf, size_t n);
long   bx_lseek(int fd, long off, int whence);
int    bx_fstat(int fd, struct bionic_stat *st);
int    bx_stat(const char *path, struct bionic_stat *st);
int    bx_access(const char *path, int mode);
int    bx_fcntl(int fd, int cmd, long arg);
int    bx_ioctl(int fd, unsigned long req, long arg);
int    bx_dup2(int a, int b);
int    bx_pipe(int fds[2]);
void  *bx_opendir(const char *path);
struct bionic_dirent *bx_readdir(void *dir);
int    bx_closedir(void *dir);
int    bx_mkdir(const char *path, unsigned mode);
int    bx_rmdir(const char *path);
int    bx_unlink(const char *path);
int    bx_remove(const char *path);
int    bx_rename(const char *a, const char *b);
int    bx_chdir(const char *path);
char  *bx_getcwd(char *buf, size_t size);
char  *bx_realpath(const char *path, char *resolved);
long   bx_readlink(const char *path, char *buf, size_t n);
char  *bx_basename(const char *path);
void  *bx_mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int    bx_munmap(void *addr, size_t len);
int    bx_mprotect(void *addr, size_t len, int prot);

/* bionic_libc.c */
void   bx_libc_init(void);
extern char **bx_environ;
extern uintptr_t bx_stack_chk_guard;
char  *bx_getenv(const char *name);
int    bx_setenv(const char *name, const char *value, int overwrite);
int    bx_clock_gettime(int clk, struct bionic_timespec *ts);
int    bx_nanosleep(const struct bionic_timespec *req, struct bionic_timespec *rem);
struct bionic_tm *bx_localtime_r(const long *t, struct bionic_tm *out);
struct bionic_tm *bx_gmtime_r(const long *t, struct bionic_tm *out);
struct bionic_tm *bx_gmtime(const long *t);
long   bx_mktime(struct bionic_tm *tm);
size_t bx_strftime(char *s, size_t max, const char *fmt, const struct bionic_tm *tm);
long   bx_times(struct bionic_tms *t);
long   bx_sysconf(int name);
int    bx_system_property_get(const char *name, char *value);
char  *bx_setlocale(int cat, const char *locale);
int    bx_strerror_r(int e, char *buf, size_t n);
int    bx_isfinitef(float f);
void   bx_abort(void) __attribute__((noreturn));
void   bx_exit(int code) __attribute__((noreturn));
void   bx_stack_chk_fail(void) __attribute__((noreturn));
int    bx_cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void   bx_cxa_finalize(void *dso);
void   bx_google_region(void);
int    bx_android_log_print(int prio, const char *tag, const char *fmt, ...);
int    bx_android_log_write(int prio, const char *tag, const char *text);
int    bx_getpid(void);
unsigned bx_geteuid(void);
void  *bx_getpwuid(unsigned uid);
int    bx_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old);
void  *bx_signal(int sig, void *handler);
int    bx_sigemptyset(unsigned long *set);
int    bx_sigaddset(unsigned long *set, int sig);
int    bx_pthread_sigmask(int how, const unsigned long *set, unsigned long *old);
int    bx_raise(int sig);
unsigned bx_alarm(unsigned s);
int    bx_system(const char *cmd);
int    bx_fork(void);
int    bx_execvp(const char *file, char *const argv[]);
int    bx_waitpid(int pid, int *status, int options);

/* bionic_net.c -- offline networking */
int    bx_socket(int domain, int type, int proto);
int    bx_net_fail(void);
int    bx_getaddrinfo(const char *node, const char *svc, const void *hints, void **res);
void   bx_freeaddrinfo(void *res);
const char *bx_gai_strerror(int e);
int    bx_gethostbyname_r(const char *name, void *ret, char *buf, size_t len, void **result, int *h_err);
void  *bx_gethostbyaddr(const void *addr, unsigned len, int type);
int    bx_gethostname(char *name, size_t len);
unsigned bx_inet_addr(const char *cp);
char  *bx_inet_ntoa(unsigned addr);
int    bx_inet_pton(int af, const char *src, void *dst);
const char *bx_inet_ntop(int af, const void *src, char *dst, unsigned size);
int    bx_select(int n, void *r, void *w, void *e, void *tv);
int    bx_poll(void *fds, unsigned long n, int timeout);

/* bionic_pthread.c */
int    bx_pthread_attr_init(bionic_attr_t *a);
int    bx_pthread_attr_destroy(bionic_attr_t *a);
int    bx_pthread_attr_setdetachstate(bionic_attr_t *a, int state);
int    bx_pthread_attr_setstacksize(bionic_attr_t *a, size_t size);
int    bx_pthread_create(bionic_pthread_t *t, const bionic_attr_t *a, void *(*fn)(void *), void *arg);
int    bx_pthread_join(bionic_pthread_t t, void **ret);
int    bx_pthread_detach(bionic_pthread_t t);
bionic_pthread_t bx_pthread_self(void);
int    bx_pthread_equal(bionic_pthread_t a, bionic_pthread_t b);
void   bx_pthread_exit(void *ret) __attribute__((noreturn));
int    bx_pthread_getschedparam(bionic_pthread_t t, int *policy, struct bionic_sched_param *p);
int    bx_pthread_setschedparam(bionic_pthread_t t, int policy, const struct bionic_sched_param *p);
int    bx_sched_get_priority_max(int policy);
int    bx_sched_get_priority_min(int policy);
int    bx_sched_yield(void);
int    bx_pthread_mutexattr_init(bionic_mutexattr_t *a);
int    bx_pthread_mutexattr_destroy(bionic_mutexattr_t *a);
int    bx_pthread_mutexattr_settype(bionic_mutexattr_t *a, int type);
int    bx_pthread_mutex_init(bionic_mutex_t *m, const bionic_mutexattr_t *a);
int    bx_pthread_mutex_destroy(bionic_mutex_t *m);
int    bx_pthread_mutex_lock(bionic_mutex_t *m);
int    bx_pthread_mutex_trylock(bionic_mutex_t *m);
int    bx_pthread_mutex_unlock(bionic_mutex_t *m);
int    bx_pthread_cond_init(bionic_cond_t *c, const bionic_condattr_t *a);
int    bx_pthread_cond_destroy(bionic_cond_t *c);
int    bx_pthread_cond_signal(bionic_cond_t *c);
int    bx_pthread_cond_broadcast(bionic_cond_t *c);
int    bx_pthread_cond_wait(bionic_cond_t *c, bionic_mutex_t *m);
int    bx_pthread_cond_timedwait(bionic_cond_t *c, bionic_mutex_t *m, const struct bionic_timespec *abstime);
int    bx_pthread_once(bionic_once_t *once, void (*fn)(void));
int    bx_pthread_key_create(bionic_key_t *key, void (*dtor)(void *));
int    bx_pthread_key_delete(bionic_key_t key);
void  *bx_pthread_getspecific(bionic_key_t key);
int    bx_pthread_setspecific(bionic_key_t key, const void *value);
int    bx_sem_init(bionic_sem_t *s, int shared, unsigned value);
int    bx_sem_destroy(bionic_sem_t *s);
int    bx_sem_post(bionic_sem_t *s);
int    bx_sem_wait(bionic_sem_t *s);
int    bx_sem_trywait(bionic_sem_t *s);
int    bx_sem_getvalue(bionic_sem_t *s, int *value);
int    bx_sem_timedwait(bionic_sem_t *s, const struct bionic_timespec *abstime);
int    bx_pthread_rwlock_init(bionic_rwlock_t *rw, const void *attr);
int    bx_pthread_rwlock_destroy(bionic_rwlock_t *rw);
int    bx_pthread_rwlock_rdlock(bionic_rwlock_t *rw);
int    bx_pthread_rwlock_wrlock(bionic_rwlock_t *rw);
int    bx_pthread_rwlock_unlock(bionic_rwlock_t *rw);
long   bx_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

/* sjlj.S */
int  bx_sigsetjmp(void *env, int savemask);
void bx_siglongjmp(void *env, int val) __attribute__((noreturn));


/* ---- bionic_extra.c: new in the Poor Bunny build ---- */
#include <time.h>
#include <wchar.h>
#include <wctype.h>

void  *bx_newlocale(int mask, const char *name, void *base);
void   bx_freelocale(void *loc);
void  *bx_uselocale(void *loc);
void  *bx_duplocale(void *loc);
struct lconv *bx_localeconv(void);
size_t bx_ctype_get_mb_cur_max(void);

int    bx_iswalpha_l(wint_t c, void *loc);
int    bx_iswblank_l(wint_t c, void *loc);
int    bx_iswcntrl_l(wint_t c, void *loc);
int    bx_iswdigit_l(wint_t c, void *loc);
int    bx_iswlower_l(wint_t c, void *loc);
int    bx_iswprint_l(wint_t c, void *loc);
int    bx_iswpunct_l(wint_t c, void *loc);
int    bx_iswspace_l(wint_t c, void *loc);
int    bx_iswupper_l(wint_t c, void *loc);
int    bx_iswxdigit_l(wint_t c, void *loc);
wint_t bx_towlower_l(wint_t c, void *loc);
wint_t bx_towupper_l(wint_t c, void *loc);

int    bx_strcoll_l(const char *a, const char *b, void *loc);
size_t bx_strxfrm_l(char *dst, const char *src, size_t n, void *loc);
int    bx_wcscoll_l(const wchar_t *a, const wchar_t *b, void *loc);
size_t bx_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
size_t bx_strftime_l(char *s, size_t max, const char *fmt, const struct tm *tm, void *loc);
long long          bx_strtoll_l(const char *s, char **end, int base, void *loc);
unsigned long long bx_strtoull_l(const char *s, char **end, int base, void *loc);
long double        bx_strtold_l(const char *s, char **end, void *loc);

size_t bx_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, mbstate_t *ps);
size_t bx_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps);

void   bx_sincos(double x, double *s, double *c);
void   bx_sincosf(float x, float *s, float *c);

void   bx_FD_SET_chk(int fd, void *set, size_t setsize);
int    bx_FD_ISSET_chk(int fd, const void *set, size_t setsize);
void   bx_assert2(const char *file, int line, const char *fn, const char *msg);
void   bx_android_set_abort_message(const char *msg);
int    bx_cxa_thread_atexit_impl(void (*fn)(void *), void *arg, void *dso);
int    bx_register_atfork(void (*p)(void), void (*pa)(void), void (*c)(void), void *dso);
unsigned long bx_getauxval(unsigned long type);
int    bx_memfd_create(const char *name, unsigned int flags);
int    bx_socketpair(int domain, int type, int protocol, int sv[2]);
int    bx_kill(int pid, int sig);
void   bx_openlog(const char *ident, int opt, int facility);
void   bx_closelog(void);
void   bx_syslog(int priority, const char *fmt, ...);

int        bx_fseeko(FILE *bf, long long off, int whence);
long long  bx_ftello(FILE *bf);
void       bx_rewind(FILE *bf);
int        bx_setvbuf(FILE *bf, char *buf, int mode, size_t size);
void      *bx_tmpfile(void);

/* Hidden behind __GNU_VISIBLE in newlib; provided by the port instead. */
char      *bx_strcasestr(const char *haystack, const char *needle);
int        bx_vasprintf(char **out, const char *fmt, va_list ap);
/* Declared by newlib but implemented only for SPU/RTEMS. */
int        bx_posix_memalign(void **memptr, size_t alignment, size_t size);

/* ---- bionic_defold.c: new in the Defold build ---- */
void  *bx_memcpy_chk(void *d, const void *s, size_t n, size_t dl);
void  *bx_memmove_chk(void *d, const void *s, size_t n, size_t dl);
void  *bx_memset_chk(void *d, int c, size_t n, size_t dl);
size_t bx_strlen_chk(const char *s, size_t sl);
char  *bx_strchr_chk(const char *s, int c, size_t sl);
char  *bx_strrchr_chk(const char *s, int c, size_t sl);
long   bx_read_chk(int fd, void *buf, size_t n, size_t bl);
int    bx_open_2(const char *path, int flags);
int    bx_vsnprintf_chk(char *buf, size_t n, int flags, size_t bl, const char *fmt, va_list ap);
int    bx_vsprintf_chk(char *buf, int flags, size_t bl, const char *fmt, va_list ap);
void   bx_FD_CLR_chk(int fd, void *set, size_t setsize);
extern char bx_ctype_[1 + 256];
extern const char *bx_ctype_ptr;
int    bx_getc(FILE *f);
int    bx_ungetc(int c, FILE *f);
int    bx_fscanf(FILE *f, const char *fmt, ...);
int    bx_vprintf(const char *fmt, va_list ap);
void  *bx_popen(const char *cmd, const char *mode);
int    bx_pclose(void *f);
int    bx_mkstemp(char *tmpl);
struct bionic_tm *bx_localtime(const long *t);
long   bx_clock(void);
int    bx_usleep(unsigned us);
int    bx_setitimer(int which, const void *nv, void *ov);
void  *bx_mremap(void *old, size_t oldsz, size_t newsz, int flags, ...);
int    bx_pthread_attr_getstacksize(const bionic_attr_t *a, size_t *size);
int    bx_pthread_getattr_np(bionic_pthread_t t, bionic_attr_t *a);
int    bx_pthread_setname_np(bionic_pthread_t t, const char *name);
int   *bx_get_h_errno(void);
void  *bx_gethostbyname(const char *name);
const char *bx_hstrerror(int e);
int    bx_inet_aton(const char *cp, void *out);
int    bx_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hl, char *serv, unsigned sl, int flags);
int    bx_dladdr(const void *addr, void *info);
void   bx_glBindVertexArrayOES(unsigned array);
void   bx_glGenVertexArraysOES(int n, unsigned *a);
void   bx_defold_init(void);

#endif
