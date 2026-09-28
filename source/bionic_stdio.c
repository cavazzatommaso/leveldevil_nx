/* bionic_stdio.c -- FILE*, file descriptors, directories, mmap.
 *
 * __sF: bionic's stdin/stdout/stderr are &__sF[0..2], three 152-byte FILE
 * structs the modules reference directly (liblime compares against
 * __sF+0x130 to detect stderr). The modules never look inside them, so the
 * shim exports a blank 456-byte array and recognizes pointers into it. Writes
 * to stdout/stderr become log lines; stdin reads as empty.
 *
 * Every path goes through path_translate() first.
 *
 * MIT licensed, see LICENSE.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "bionic.h"
#include "log.h"
#include "paths.h"

char bx_sF[3 * BIONIC_FILE_SIZE];

/* --------------------------------------------------------- std streams -- */

typedef struct {
    RMutex lock;
    size_t len;
    char   buf[1024];
} LineSink;

static LineSink g_sinks[3];

static int std_index(const FILE *f)
{
    const char *p = (const char *)f;
    if (p >= bx_sF && p < bx_sF + sizeof(bx_sF))
        return (int)((p - bx_sF) / BIONIC_FILE_SIZE);
    return -1;
}

static void sink_write(int idx, const char *data, size_t n)
{
    LineSink *s = &g_sinks[idx == 2 ? 2 : 1];
    size_t i;
    rmutexLock(&s->lock);
    for (i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || s->len == sizeof(s->buf) - 1) {
            s->buf[s->len] = '\0';
            if (s->len)
                log_printf("[%s] %s", idx == 2 ? "stderr" : "stdout", s->buf);
            s->len = 0;
            if (c == '\n')
                continue;
        }
        if (c != '\r')
            s->buf[s->len++] = c;
    }
    rmutexUnlock(&s->lock);
}

FILE *bx_real_file(FILE *f)
{
    switch (std_index(f)) {
    case 0: return stdin;
    case 1: return stdout;
    case 2: return stderr;
    default: return f;
    }
}

static void filter_mode(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (; *in && j + 1 < n; in++) {
        /* bionic accepts 'e' (O_CLOEXEC) and 'm'; newlib does not */
        if (strchr("rwab+xt", *in))
            out[j++] = *in;
    }
    out[j] = '\0';
}

static int is_virtual_system_path(const char *p)
{
    return !strncmp(p, "/proc/", 6) || !strncmp(p, "/dev/", 5) ||
           !strncmp(p, "/sys/", 5) || !strncmp(p, "/system/", 8) ||
           !strncmp(p, "/vendor/", 8);
}

FILE *bx_fopen(const char *path, const char *mode)
{
    char p[1024], m[8];
    FILE *f;
    if (!path || !mode) {
        errno = LX_EINVAL;
        return NULL;
    }
    path_translate(path, p, sizeof(p));
    if (is_virtual_system_path(p)) {
        LOGD("fopen(%s): Android system path, not available", p);
        errno = LX_ENOENT;
        return NULL;
    }
    filter_mode(mode, m, sizeof(m));
    f = fopen(p, m);
    if (!f) {
        int e = errno;
        bx_fix_errno();
        LOGD("fopen(%s, %s) failed, errno %d", p, mode, e);
    } else {
        LOGD("fopen(%s, %s) = %p", p, mode, (void *)f);
    }
    return f;
}

FILE *bx_fdopen(int fd, const char *mode)
{
    char m[8];
    FILE *f;
    if (fd >= 0 && fd <= 2)
        return (FILE *)(bx_sF + fd * BIONIC_FILE_SIZE);
    filter_mode(mode ? mode : "r", m, sizeof(m));
    f = fdopen(fd, m);
    if (!f)
        bx_fix_errno();
    return f;
}

int bx_fclose(FILE *f)
{
    if (!f || std_index(f) >= 0)
        return 0;
    return fclose(f);
}

size_t bx_fread(void *p, size_t sz, size_t n, FILE *f)
{
    if (!f || std_index(f) >= 0)
        return 0;
    return fread(p, sz, n, f);
}

size_t bx_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    int i = f ? std_index(f) : -1;
    if (i >= 1) {
        sink_write(i, p, sz * n);
        return n;
    }
    if (!f || i == 0)
        return 0;
    return fwrite(p, sz, n, f);
}

int bx_fseek(FILE *f, long off, int whence)
{
    if (!f || std_index(f) >= 0) {
        errno = LX_EBADF;
        return -1;
    }
    return fseek(f, off, whence);
}

long bx_ftell(FILE *f)
{
    if (!f || std_index(f) >= 0)
        return 0;
    return ftell(f);
}

int bx_fflush(FILE *f)
{
    if (f && std_index(f) >= 0)
        return 0;
    return fflush(f);
}

int bx_feof(FILE *f)     { return (!f || std_index(f) >= 0) ? (std_index(f) == 0) : feof(f); }
int bx_ferror(FILE *f)   { return (!f || std_index(f) >= 0) ? 0 : ferror(f); }
void bx_clearerr(FILE *f) { if (f && std_index(f) < 0) clearerr(f); }

int bx_fileno(FILE *f)
{
    int i = f ? std_index(f) : -1;
    if (i >= 0)
        return i;
    return f ? fileno(f) : -1;
}

char *bx_fgets(char *s, int n, FILE *f)
{
    if (!f || std_index(f) >= 0)
        return NULL;
    return fgets(s, n, f);
}

int bx_fgetc(FILE *f)
{
    if (!f || std_index(f) >= 0)
        return EOF;
    return fgetc(f);
}

int bx_fputc(int c, FILE *f)
{
    int i = f ? std_index(f) : -1;
    if (i >= 1) {
        char ch = (char)c;
        sink_write(i, &ch, 1);
        return (unsigned char)c;
    }
    if (!f || i == 0)
        return EOF;
    return fputc(c, f);
}

int bx_fputs(const char *s, FILE *f)
{
    int i = f ? std_index(f) : -1;
    if (i >= 1) {
        sink_write(i, s, strlen(s));
        return 0;
    }
    if (!f || i == 0)
        return EOF;
    return fputs(s, f);
}

int bx_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    int i = f ? std_index(f) : -1;
    if (i >= 1) {
        char small[1024];
        va_list ap2;
        int n;
        va_copy(ap2, ap);
        n = vsnprintf(small, sizeof(small), fmt, ap2);
        va_end(ap2);
        if (n < 0)
            return n;
        if ((size_t)n < sizeof(small)) {
            sink_write(i, small, (size_t)n);
        } else {
            char *big = malloc((size_t)n + 1);
            if (big) {
                vsnprintf(big, (size_t)n + 1, fmt, ap);
                sink_write(i, big, (size_t)n);
                free(big);
            }
        }
        return n;
    }
    if (!f || i == 0)
        return -1;
    return vfprintf(f, fmt, ap);
}

int bx_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = bx_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int bx_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = bx_vfprintf((FILE *)(bx_sF + BIONIC_FILE_SIZE), fmt, ap);
    va_end(ap);
    return n;
}

int bx_puts(const char *s)
{
    sink_write(1, s, strlen(s));
    sink_write(1, "\n", 1);
    return 1;
}

int bx_putchar(int c)
{
    char ch = (char)c;
    sink_write(1, &ch, 1);
    return (unsigned char)c;
}

int bx_getchar(void) { return EOF; }

void bx_setbuf(FILE *f, char *buf)
{
    if (f && std_index(f) < 0)
        setbuf(f, buf);
}

/* ---------------------------------------------------------------- fds -- */

int bx_open(const char *path, int flags, int mode)
{
    char p[1024];
    int nf, fd;
    struct stat st;

    if (!path) {
        errno = LX_EINVAL;
        return -1;
    }
    path_translate(path, p, sizeof(p));
    if (is_virtual_system_path(p)) {
        errno = LX_ENOENT;
        return -1;
    }
    switch (flags & LX_O_ACCMODE) {
    case 0:  nf = O_RDONLY; break;
    case 1:  nf = O_WRONLY; break;
    default: nf = O_RDWR;   break;
    }
    if (flags & LX_O_CREAT)  nf |= O_CREAT;
    if (flags & LX_O_EXCL)   nf |= O_EXCL;
    if (flags & LX_O_TRUNC)  nf |= O_TRUNC;
    if (flags & LX_O_APPEND) nf |= O_APPEND;
    if (flags & LX_O_DIRECTORY) {
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode))
            errno = LX_ENOTDIR;
        else
            errno = LX_EOPNOTSUPP;
        return -1;
    }
    fd = open(p, nf, mode ? mode : 0666);
    if (fd < 0)
        bx_fix_errno();
    LOGD("open(%s, 0%o) = %d", p, flags, fd);
    return fd;
}

int bx_close(int fd)
{
    if (fd >= 0 && fd <= 2)
        return 0;
    return close(fd);
}

long bx_read(int fd, void *buf, size_t n)
{
    long r;
    if (fd == 0)
        return 0;
    r = read(fd, buf, n);
    if (r < 0)
        bx_fix_errno();
    return r;
}

long bx_write(int fd, const void *buf, size_t n)
{
    long r;
    if (fd == 1 || fd == 2) {
        sink_write(fd, buf, n);
        return (long)n;
    }
    r = write(fd, buf, n);
    if (r < 0)
        bx_fix_errno();
    return r;
}

long bx_lseek(int fd, long off, int whence)
{
    long r;
    if (fd >= 0 && fd <= 2) {
        errno = 29; /* ESPIPE */
        return -1;
    }
    r = lseek(fd, off, whence);
    if (r < 0)
        bx_fix_errno();
    return r;
}

static void conv_stat(const struct stat *s, struct bionic_stat *b)
{
    memset(b, 0, sizeof(*b));
    b->st_dev = (uint64_t)s->st_dev;
    b->st_ino = (uint64_t)s->st_ino;
    b->st_mode = (uint32_t)s->st_mode;
    b->st_nlink = s->st_nlink ? (uint32_t)s->st_nlink : 1;
    b->st_uid = (uint32_t)s->st_uid;
    b->st_gid = (uint32_t)s->st_gid;
    b->st_size = (int64_t)s->st_size;
    b->st_blksize = s->st_blksize ? (int32_t)s->st_blksize : 4096;
    b->st_blocks = ((int64_t)s->st_size + 511) / 512;
    b->st_atim.tv_sec = (long)s->st_atime;
    b->st_mtim.tv_sec = (long)s->st_mtime;
    b->st_ctim.tv_sec = (long)s->st_ctime;
}

int bx_fstat(int fd, struct bionic_stat *st)
{
    struct stat s;
    if (!st) {
        errno = LX_EINVAL;
        return -1;
    }
    if (fd >= 0 && fd <= 2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666;   /* character device */
        st->st_blksize = 1024;
        return 0;
    }
    if (fstat(fd, &s) != 0) {
        bx_fix_errno();
        return -1;
    }
    conv_stat(&s, st);
    return 0;
}

int bx_stat(const char *path, struct bionic_stat *st)
{
    char p[1024];
    struct stat s;
    if (!path || !st) {
        errno = LX_EINVAL;
        return -1;
    }
    path_translate(path, p, sizeof(p));
    if (stat(p, &s) != 0) {
        bx_fix_errno();
        return -1;
    }
    conv_stat(&s, st);
    return 0;
}

int bx_access(const char *path, int mode)
{
    char p[1024];
    struct stat s;
    (void)mode;
    if (!path) {
        errno = LX_EINVAL;
        return -1;
    }
    path_translate(path, p, sizeof(p));
    /* Horizon's access() is unreliable on files open for writing; stat is not */
    if (stat(p, &s) != 0) {
        bx_fix_errno();
        return -1;
    }
    return 0;
}

int bx_fcntl(int fd, int cmd, long arg)
{
    (void)fd; (void)arg;
    switch (cmd) {
    case 1:  return 0;             /* F_GETFD */
    case 2:  return 0;             /* F_SETFD */
    case 3:  return LX_O_RDWR;     /* F_GETFL */
    case 4:  return 0;             /* F_SETFL */
    default:
        errno = LX_EINVAL;
        return -1;
    }
}

int bx_ioctl(int fd, unsigned long req, long arg)
{
    (void)fd; (void)req; (void)arg;
    errno = LX_ENOTTY;
    return -1;
}

int bx_dup2(int a, int b)
{
    (void)a; (void)b;
    errno = LX_EBADF;
    return -1;
}

int bx_pipe(int fds[2])
{
    (void)fds;
    errno = LX_EMFILE;
    return -1;
}

/* ---------------------------------------------------------- directories -- */

#define BXDIR_MAGIC 0x52494442u

typedef struct {
    uint32_t magic;
    DIR *dir;
    struct bionic_dirent ent;
} BxDir;

void *bx_opendir(const char *path)
{
    char p[1024];
    DIR *d;
    BxDir *bd;
    if (!path) {
        errno = LX_EINVAL;
        return NULL;
    }
    path_translate(path, p, sizeof(p));
    d = opendir(p);
    if (!d) {
        bx_fix_errno();
        return NULL;
    }
    bd = calloc(1, sizeof(*bd));
    if (!bd) {
        closedir(d);
        errno = LX_ENOMEM;
        return NULL;
    }
    bd->magic = BXDIR_MAGIC;
    bd->dir = d;
    return bd;
}

struct bionic_dirent *bx_readdir(void *dirp)
{
    BxDir *bd = dirp;
    struct dirent *e;
    if (!bd || bd->magic != BXDIR_MAGIC) {
        errno = LX_EBADF;
        return NULL;
    }
    e = readdir(bd->dir);
    if (!e)
        return NULL;
    memset(&bd->ent, 0, sizeof(bd->ent));
    bd->ent.d_ino = (uint64_t)e->d_ino;
    bd->ent.d_type = (uint8_t)e->d_type;
    bd->ent.d_reclen = sizeof(bd->ent);
    snprintf(bd->ent.d_name, sizeof(bd->ent.d_name), "%s", e->d_name);
    return &bd->ent;
}

int bx_closedir(void *dirp)
{
    BxDir *bd = dirp;
    if (!bd || bd->magic != BXDIR_MAGIC) {
        errno = LX_EBADF;
        return -1;
    }
    closedir(bd->dir);
    bd->magic = 0;
    free(bd);
    return 0;
}

#define PATH_OP1(fn, call)                                   \
    int bx_##fn(const char *path)                            \
    {                                                        \
        char p[1024];                                        \
        int r;                                               \
        if (!path) {                                         \
            errno = LX_EINVAL;                               \
            return -1;                                       \
        }                                                    \
        path_translate(path, p, sizeof(p));                  \
        r = call;                                            \
        if (r != 0)                                          \
            bx_fix_errno();                                  \
        LOGD(#fn "(%s) = %d", p, r);                         \
        return r;                                            \
    }

PATH_OP1(rmdir, rmdir(p))
PATH_OP1(unlink, unlink(p))
PATH_OP1(remove, remove(p))
PATH_OP1(chdir, chdir(p))

int bx_mkdir(const char *path, unsigned mode)
{
    char p[1024];
    int r;
    if (!path) {
        errno = LX_EINVAL;
        return -1;
    }
    path_translate(path, p, sizeof(p));
    r = mkdir(p, (mode_t)mode);
    if (r != 0)
        bx_fix_errno();
    LOGD("mkdir(%s) = %d", p, r);
    return r;
}

int bx_rename(const char *a, const char *b)
{
    char pa[1024], pb[1024];
    int r;
    if (!a || !b) {
        errno = LX_EINVAL;
        return -1;
    }
    path_translate(a, pa, sizeof(pa));
    path_translate(b, pb, sizeof(pb));
    r = rename(pa, pb);
    if (r != 0) {
        /* FAT/exFAT via fsdev refuses to rename over an existing file;
         * Android's rename replaces it. SharedObject saves rely on that. */
        struct stat st;
        if (stat(pb, &st) == 0 && S_ISREG(st.st_mode) && unlink(pb) == 0)
            r = rename(pa, pb);
        if (r != 0)
            bx_fix_errno();
    }
    LOGD("rename(%s, %s) = %d", pa, pb, r);
    return r;
}

char *bx_getcwd(char *buf, size_t size)
{
    char *r = getcwd(buf, size);
    if (!r)
        bx_fix_errno();
    return r;
}

char *bx_realpath(const char *path, char *resolved)
{
    char tmp[1024], p[1024];
    struct stat st;
    if (!path) {
        errno = LX_EINVAL;
        return NULL;
    }
    if (path[0] != '/' && !strchr(path, ':')) {
        char cwd[512];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), "%s", paths_assets());
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", path);
    }
    path_translate(tmp, p, sizeof(p));
    if (stat(p, &st) != 0) {
        bx_fix_errno();
        return NULL;
    }
    if (!resolved)
        return strdup(p);
    snprintf(resolved, 4096, "%s", p);
    return resolved;
}

long bx_readlink(const char *path, char *buf, size_t n)
{
    (void)path; (void)buf; (void)n;
    errno = LX_EINVAL;
    return -1;
}

char *bx_basename(const char *path)
{
    static __thread char out[256];
    const char *end, *start;
    size_t len;
    if (!path || !*path) {
        strcpy(out, ".");
        return out;
    }
    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/')
        end--;
    start = end;
    while (start > path && start[-1] != '/')
        start--;
    len = (size_t)(end - start);
    if (len >= sizeof(out))
        len = sizeof(out) - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    if (!len)
        strcpy(out, "/");
    return out;
}

/* ----------------------------------------------------------------- mmap -- */

#define LX_MAP_FIXED     0x10
#define LX_MAP_ANONYMOUS 0x20

typedef struct MapEnt {
    void *addr;
    size_t len;
    struct MapEnt *next;
} MapEnt;

static MapEnt *g_maps;
static Mutex g_maps_lock;

void *bx_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    size_t alen;
    void *mem;
    MapEnt *e;
    (void)addr; (void)prot;

    if (!len) {
        errno = LX_EINVAL;
        return (void *)-1;
    }
    if (flags & LX_MAP_FIXED) {
        LOGE("mmap: MAP_FIXED at %p not supported", addr);
        errno = LX_ENOMEM;
        return (void *)-1;
    }
    alen = (len + 0xFFF) & ~(size_t)0xFFF;
    mem = memalign(0x1000, alen);
    e = malloc(sizeof(*e));
    if (!mem || !e) {
        free(mem);
        free(e);
        errno = LX_ENOMEM;
        return (void *)-1;
    }
    memset(mem, 0, alen);
    if (!(flags & LX_MAP_ANONYMOUS) && fd >= 0) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        size_t got = 0;
        if (lseek(fd, (off_t)off, SEEK_SET) >= 0) {
            while (got < len) {
                ssize_t r = read(fd, (char *)mem + got, len - got);
                if (r <= 0)
                    break;
                got += (size_t)r;
            }
        }
        if (cur >= 0)
            lseek(fd, cur, SEEK_SET);
        LOGD("mmap: file fd %d, %zu of %zu bytes copied", fd, got, len);
    }
    e->addr = mem;
    e->len = alen;
    mutexLock(&g_maps_lock);
    e->next = g_maps;
    g_maps = e;
    mutexUnlock(&g_maps_lock);
    return mem;
}

int bx_munmap(void *addr, size_t len)
{
    MapEnt **pp, *e = NULL;
    (void)len;
    mutexLock(&g_maps_lock);
    for (pp = &g_maps; *pp; pp = &(*pp)->next) {
        if ((*pp)->addr == addr) {
            e = *pp;
            *pp = e->next;
            break;
        }
    }
    mutexUnlock(&g_maps_lock);
    if (e) {
        free(e->addr);
        free(e);
    }
    return 0;
}
