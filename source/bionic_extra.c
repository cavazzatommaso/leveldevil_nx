/* bionic_extra.c -- the imports this build needs that Total Party Kill's did not.
 *
 * Poor Bunny's libraries import 523 distinct symbols against the reference
 * port's 456. Most of the 102 new ones fall into three groups, and this file
 * is all three:
 *
 *  1. The locale-suffixed C functions (strcoll_l, iswdigit_l, strftime_l,
 *     wcstold_l, ...). A newer NDK's <locale.h> hands these out wherever the
 *     plain form was used before. The port runs in one locale and sets
 *     LANG=en_US.UTF-8, so each one drops the locale_t and calls the plain
 *     form. That is exactly right for "C" and close enough for en_US, and it
 *     avoids depending on which of these devkitPro's newlib happens to
 *     export.
 *
 *  2. Wide-character conversion (mbsnrtowcs, wcsnrtombs, wcstoll, ...).
 *     Implemented over newlib's own multibyte machinery where it exists.
 *
 *  3. Small shims for things that have no meaning here: fork handlers, the
 *     syslog family, getauxval, memfd_create, socketpair, kill.
 *
 * Also here: stdin/stdout/stderr. This NDK exports them as FILE* variables
 * rather than the old __sF[] macro, so libApplicationMain imports three data
 * symbols that must be pointers into the fake FILE array bionic_stdio.c owns.
 *
 * MIT licensed, see LICENSE.
 */
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include "bionic.h"
#include "log.h"

/* ------------------------------------------------- stdin/stdout/stderr --- */
/* bionic's FILE for fd N lives at bx_sF + N * BIONIC_FILE_SIZE (see
 * bionic_stdio.c). These three are the pointer variables the modules import;
 * the addresses are compile-time constants, so no initialisation runs. */

extern char bx_sF[];

void *bx_stdin_var  = bx_sF + 0 * BIONIC_FILE_SIZE;
void *bx_stdout_var = bx_sF + 1 * BIONIC_FILE_SIZE;
void *bx_stderr_var = bx_sF + 2 * BIONIC_FILE_SIZE;

/* ------------------------------------------------------------- locale ---- */
/* A locale_t here is a token, not a locale. Nothing reads it. */

static int g_locale_token = 0x10CA1E;

void *bx_newlocale(int mask, const char *name, void *base)
{
    (void)mask; (void)name; (void)base;
    return &g_locale_token;
}

void  bx_freelocale(void *loc)      { (void)loc; }
void *bx_uselocale(void *loc)       { (void)loc; return &g_locale_token; }
void *bx_duplocale(void *loc)       { (void)loc; return &g_locale_token; }

struct lconv *bx_localeconv(void)   { return localeconv(); }

size_t bx_ctype_get_mb_cur_max(void) { return MB_CUR_MAX; }

#define WCTYPE_L(name) \
    int bx_##name##_l(wint_t c, void *loc) { (void)loc; return name(c); }

WCTYPE_L(iswalpha)
WCTYPE_L(iswcntrl)
WCTYPE_L(iswdigit)
WCTYPE_L(iswlower)
WCTYPE_L(iswprint)
WCTYPE_L(iswpunct)
WCTYPE_L(iswspace)
WCTYPE_L(iswupper)
WCTYPE_L(iswxdigit)

/* iswblank is C99 but not always wired into newlib's wctype table. */
int bx_iswblank_l(wint_t c, void *loc)
{
    (void)loc;
    return c == L' ' || c == L'\t';
}

wint_t bx_towlower_l(wint_t c, void *loc) { (void)loc; return towlower(c); }
wint_t bx_towupper_l(wint_t c, void *loc) { (void)loc; return towupper(c); }

int    bx_strcoll_l(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t bx_strxfrm_l(char *dst, const char *src, size_t n, void *loc)
{
    (void)loc;
    return strxfrm(dst, src, n);
}
int    bx_wcscoll_l(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t bx_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *loc)
{
    (void)loc;
    return wcsxfrm(dst, src, n);
}

size_t bx_strftime_l(char *s, size_t max, const char *fmt, const struct tm *tm, void *loc)
{
    (void)loc;
    return strftime(s, max, fmt, tm);
}

long long      bx_strtoll_l(const char *s, char **end, int base, void *loc)  { (void)loc; return strtoll(s, end, base); }
unsigned long long bx_strtoull_l(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
long double    bx_strtold_l(const char *s, char **end, void *loc)            { (void)loc; return strtold(s, end); }

/* ---------------------------------------------------------- wide chars --- */

/* Neither of these is in every newlib; both are simple loops over the
 * restartable single-character conversions. */
size_t bx_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, mbstate_t *ps)
{
    static mbstate_t local;
    const char *s = *src;
    size_t made = 0;

    if (!ps)
        ps = &local;
    while ((!dst || made < len) && nms > 0) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, s, nms, ps);
        if (n == (size_t)-1 || n == (size_t)-2)
            return (size_t)-1;
        if (n == 0) {                 /* the NUL terminates and is not counted */
            if (dst)
                dst[made] = 0;
            *src = NULL;
            return made;
        }
        if (dst)
            dst[made] = wc;
        made++;
        s += n;
        nms -= n;
    }
    if (dst)
        *src = s;
    return made;
}

size_t bx_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, mbstate_t *ps)
{
    static mbstate_t local;
    const wchar_t *w = *src;
    size_t made = 0;
    char buf[MB_LEN_MAX];

    if (!ps)
        ps = &local;
    while (nwc > 0) {
        size_t n = wcrtomb(buf, *w, ps);
        if (n == (size_t)-1)
            return (size_t)-1;
        if (dst && made + n > len)
            break;
        if (dst)
            memcpy(dst + made, buf, n);
        made += n;
        if (*w == 0) {
            *src = NULL;
            return made - 1;          /* the NUL is not counted */
        }
        w++;
        nwc--;
    }
    if (dst)
        *src = w;
    return made;
}

/* ----------------------------------------------------------- math bits --- */

void bx_sincos(double x, double *s, double *c)
{
    if (s) *s = __builtin_sin(x);
    if (c) *c = __builtin_cos(x);
}

void bx_sincosf(float x, float *s, float *c)
{
    if (s) *s = __builtin_sinf(x);
    if (c) *c = __builtin_cosf(x);
}

/* --------------------------------------------- things with no meaning ---- */

/* bionic's fortified FD_SET/FD_ISSET. Only select() paths reach these, and
 * networking is off, but they are linked BIND_NOW so they need addresses. */
void bx_FD_SET_chk(int fd, void *set, size_t setsize)
{
    (void)setsize;
    if (fd >= 0 && set)
        ((unsigned char *)set)[fd / 8] |= (unsigned char)(1u << (fd % 8));
}

int bx_FD_ISSET_chk(int fd, const void *set, size_t setsize)
{
    (void)setsize;
    if (fd < 0 || !set)
        return 0;
    return (((const unsigned char *)set)[fd / 8] >> (fd % 8)) & 1;
}

void bx_assert2(const char *file, int line, const char *fn, const char *msg)
{
    LOGE("assertion failed: %s (%s:%d in %s)", msg ? msg : "?",
         file ? file : "?", line, fn ? fn : "?");
    log_flush();
    abort();
}

void bx_android_set_abort_message(const char *msg)
{
    if (msg)
        LOGE("abort message: %s", msg);
}

/* hxcpp registers thread-local destructors through this. Threads here outlive
 * the process, so the destructor would never run anyway. */
int bx_cxa_thread_atexit_impl(void (*fn)(void *), void *arg, void *dso)
{
    (void)fn; (void)arg; (void)dso;
    return 0;
}

int bx_register_atfork(void (*prepare)(void), void (*parent)(void),
                       void (*child)(void), void *dso)
{
    (void)prepare; (void)parent; (void)child; (void)dso;
    return 0;                       /* nothing forks */
}

unsigned long bx_getauxval(unsigned long type)
{
    switch (type) {
    case 6:   return 0x1000;        /* AT_PAGESZ */
    case 16:  return 0;             /* AT_HWCAP: claim no optional features */
    case 26:  return 0;             /* AT_HWCAP2 */
    default:  break;
    }
    LOG_ONCE("getauxval(%lu) -> 0", type);
    return 0;
}

int bx_memfd_create(const char *name, unsigned int flags)
{
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
}

int bx_socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)domain; (void)type; (void)protocol; (void)sv;
    errno = EAFNOSUPPORT;
    return -1;
}

int bx_kill(int pid, int sig)
{
    (void)pid;
    LOG_ONCE("kill(signal %d) ignored", sig);
    return 0;
}

void bx_openlog(const char *ident, int opt, int facility)
{
    (void)ident; (void)opt; (void)facility;
}

void bx_closelog(void) { }

void bx_syslog(int priority, const char *fmt, ...)
{
    va_list ap;
    (void)priority;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------- stdio on bionic FILE -- */
/* These take the modules' FILE*, not newlib's, so each one has to go through
 * bionic_stdio.c's translation first. */

int bx_fseeko(FILE *bf, long long off, int whence)
{
    FILE *f = bx_real_file(bf);
    return f ? fseek(f, (long)off, whence) : -1;
}

long long bx_ftello(FILE *bf)
{
    FILE *f = bx_real_file(bf);
    return f ? (long long)ftell(f) : -1;
}

void bx_rewind(FILE *bf)
{
    FILE *f = bx_real_file(bf);
    if (f)
        rewind(f);
}

int bx_setvbuf(FILE *bf, char *buf, int mode, size_t size)
{
    FILE *f = bx_real_file(bf);
    return f ? setvbuf(f, buf, mode, size) : -1;
}

void *bx_tmpfile(void)
{
    LOG_ONCE("tmpfile() is not available on this port; returning NULL");
    return NULL;
}

/* ------------------------------------------------- GNU-only extensions --- */
/* devkitPro's newlib hides these behind __GNU_VISIBLE, which is 0 unless
 * _GNU_SOURCE is defined -- and the devkitPro Makefile does not define it.
 * They are implemented here rather than by widening the feature macros for
 * the whole port, because _GNU_SOURCE also swaps basename() for the GNU
 * variant and changes strerror_r's signature, and because a declaration
 * being visible still would not guarantee the symbol is built into this
 * toolchain's libc.a.
 *
 * Both allocate with newlib's malloc, which is what imports.c hands the
 * modules as "malloc"/"free", so a buffer returned here can be freed by the
 * game. */

char *bx_strcasestr(const char *haystack, const char *needle)
{
    size_t n;

    if (!haystack || !needle)
        return NULL;
    if (!*needle)
        return (char *)haystack;
    n = strlen(needle);
    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < n; i++) {
            int a = tolower((unsigned char)haystack[i]);
            int b = tolower((unsigned char)needle[i]);
            if (a != b || !haystack[i])
                break;
        }
        if (i == n)
            return (char *)haystack;
    }
    return NULL;
}

int bx_vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list measure;
    char *buf;
    int n;

    if (!out)
        return -1;
    *out = NULL;
    if (!fmt)
        return -1;

    /* C99 lets vsnprintf measure with a null destination and size 0. */
    va_copy(measure, ap);
    n = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (n < 0)
        return -1;

    buf = malloc((size_t)n + 1);
    if (!buf)
        return -1;
    n = vsnprintf(buf, (size_t)n + 1, fmt, ap);
    if (n < 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    return n;
}

/* newlib DECLARES posix_memalign in <stdlib.h> under __POSIX_VISIBLE, so it
 * compiles, but it only IMPLEMENTS it for the SPU and RTEMS targets -- on
 * aarch64-none-elf the symbol does not exist and the failure lands at link
 * time instead. memalign() is real, and a block from it is freeable with
 * free(), which is what the modules are given for "free".
 *
 * Unlike most of libc this returns the error number rather than setting
 * errno, and those numbers are read by the game, so they have to be Linux's
 * rather than newlib's. */
int bx_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *p;

    if (!memptr)
        return LX_EINVAL;
    /* Must be a power of two and a whole number of pointers. */
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return LX_EINVAL;
    if (size == 0) {
        *memptr = NULL;
        return 0;
    }
    p = memalign(alignment, size);
    if (!p)
        return LX_ENOMEM;
    *memptr = p;
    return 0;
}
