/* bionic_net.c -- networking, deliberately offline.
 *
 * The imports come from hxcpp's sys.net and Lime's curl-backed HTTPRequest.
 * The only network users in this APK are the ad, billing and Play Games SDKs,
 * which live on the Java side and are replaced by inert stand-ins. Every call
 * here fails the way an Android device in airplane mode fails, which is a
 * code path the game already handles.
 *
 * MIT licensed, see LICENSE.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "log.h"

#define LX_AF_INET  2
#define LX_AF_INET6 10
#define LX_EAI_FAIL 4

int bx_socket(int domain, int type, int proto)
{
    LOG_ONCE("network: socket(%d, %d, %d) refused, this port runs offline", domain, type, proto);
    errno = LX_EAFNOSUPPORT;
    return -1;
}

/* accept, bind, connect, listen, recv, recvfrom, send, sendto, setsockopt,
 * getsockopt, shutdown, getpeername, getsockname: nothing can reach these
 * without a socket, so one failing implementation serves all of them. */
int bx_net_fail(void)
{
    errno = LX_ENETDOWN;
    return -1;
}

int bx_getaddrinfo(const char *node, const char *svc, const void *hints, void **res)
{
    (void)svc; (void)hints;
    LOGD("network: getaddrinfo(%s) -> offline", node ? node : "");
    if (res)
        *res = NULL;
    return LX_EAI_FAIL;
}

void bx_freeaddrinfo(void *res) { (void)res; }

const char *bx_gai_strerror(int e)
{
    (void)e;
    return "Network unavailable (Nintendo Switch port runs offline)";
}

int bx_gethostbyname_r(const char *name, void *ret, char *buf, size_t len,
                       void **result, int *h_err)
{
    (void)name; (void)ret; (void)buf; (void)len;
    if (result)
        *result = NULL;
    if (h_err)
        *h_err = 1;                     /* HOST_NOT_FOUND */
    return 0;
}

void *bx_gethostbyaddr(const void *addr, unsigned len, int type)
{
    (void)addr; (void)len; (void)type;
    return NULL;
}

int bx_gethostname(char *name, size_t len)
{
    if (!name || !len) {
        errno = LX_EINVAL;
        return -1;
    }
    snprintf(name, len, "nintendo-switch");
    return 0;
}

static int parse_ipv4(const char *s, unsigned char out[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return 0;
        }
        if (!digits)
            return 0;
        out[i] = (unsigned char)v;
        if (i < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    return *s == '\0';
}

unsigned bx_inet_addr(const char *cp)
{
    unsigned char b[4];
    if (!cp || !parse_ipv4(cp, b))
        return 0xFFFFFFFFu;             /* INADDR_NONE */
    return (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
}

char *bx_inet_ntoa(unsigned addr)
{
    static __thread char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", addr & 0xFF, (addr >> 8) & 0xFF,
             (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
    return buf;
}

int bx_inet_pton(int af, const char *src, void *dst)
{
    if (af == LX_AF_INET) {
        unsigned char b[4];
        if (!src || !parse_ipv4(src, b))
            return 0;
        memcpy(dst, b, 4);
        return 1;
    }
    if (af == LX_AF_INET6)
        return 0;                       /* not parsed; callers treat as invalid */
    errno = LX_EAFNOSUPPORT;
    return -1;
}

const char *bx_inet_ntop(int af, const void *src, char *dst, unsigned size)
{
    if (af == LX_AF_INET && src && dst) {
        const unsigned char *b = src;
        snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return dst;
    }
    errno = LX_EAFNOSUPPORT;
    return NULL;
}

struct bionic_timeval { long tv_sec; long tv_usec; };

int bx_select(int n, void *r, void *w, void *e, void *tvp)
{
    const struct bionic_timeval *tv = tvp;
    (void)n; (void)r; (void)w; (void)e;
    if (tv) {
        s64 ns = (s64)tv->tv_sec * 1000000000LL + (s64)tv->tv_usec * 1000LL;
        if (ns > 100000000LL)
            ns = 100000000LL;
        if (ns > 0)
            svcSleepThread(ns);
    }
    return 0;
}

int bx_poll(void *fds, unsigned long n, int timeout)
{
    (void)fds; (void)n;
    if (timeout > 0)
        svcSleepThread((s64)(timeout > 100 ? 100 : timeout) * 1000000LL);
    return 0;
}
