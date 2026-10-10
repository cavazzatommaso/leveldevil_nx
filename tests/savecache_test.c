/* Host check for savecache.c: the exact sequence Defold's sys.save uses.
 *   cc -Ifake -I../source savecache_test.c ../source/savecache.c -lpthread && ./a.out */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "savecache.h"

#include <stdarg.h>

static char g_dir[256];
const char *paths_save_nodev(void) { return g_dir; }
int  log_get_level(void) { return 0; }
void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void save(const char *path, const char *text)    /* sys.save */
{
    char tmp[512];
    FILE *f;
    int r;
    snprintf(tmp, sizeof(tmp), "%s.defoldtmp_1", path);
    f = savecache_open_write(tmp);
    assert(f);
    fputs(text, f);
    assert(savecache_close(f, &r) && r == 0);
    assert(savecache_rename(tmp, path));
}

static char *read_cached(const char *path)
{
    static char buf[256];
    size_t n;
    int r;
    FILE *f = savecache_open_read(path);
    assert(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    assert(savecache_close(f, &r));
    return buf;
}

static char *read_card(const char *path)
{
    static char buf[256];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(void)
{
    char save_path[300], other[300];
    size_t size = 0;

    snprintf(g_dir, sizeof(g_dir), "/tmp/savecache_test_%d", (int)getpid());
    mkdir(g_dir, 0777);
    snprintf(save_path, sizeof(save_path), "%s/save", g_dir);
    snprintf(other, sizeof(other), "/tmp/elsewhere");

    assert(savecache_handles(save_path) && !savecache_handles(other));
    assert(savecache_stat(save_path, NULL) == 0);           /* unknown before */

    save(save_path, "level=1");
    save(save_path, "level=2");                              /* latest wins */
    assert(savecache_stat(save_path, &size) == 1 && size == 7);
    assert(!strcmp(read_cached(save_path), "level=2"));

    savecache_flush();
    assert(read_card(save_path) && !strcmp(read_card(save_path), "level=2"));
    {   /* no temp files left behind */
        char tmp[400];
        snprintf(tmp, sizeof(tmp), "%s.swtmp", save_path);
        assert(access(tmp, F_OK) != 0);
        snprintf(tmp, sizeof(tmp), "%s.defoldtmp_1", save_path);
        assert(access(tmp, F_OK) != 0);
    }

    save(save_path, "");                                     /* empty file */
    assert(savecache_stat(save_path, &size) == 1 && size == 0);
    assert(!strcmp(read_cached(save_path), ""));

    savecache_unlink(save_path);
    assert(savecache_stat(save_path, NULL) == -1);           /* deleted, pending */
    savecache_flush();
    assert(access(save_path, F_OK) != 0);
    assert(savecache_stat(save_path, NULL) == 0);            /* marker freed */

    {   /* many saves under fresh temp names, as Defold does: nothing
         * accumulates in the table or on the card */
        char tmp[400];
        int i;
        for (i = 0; i < 200; i++) {
            FILE *f;
            int r;
            snprintf(tmp, sizeof(tmp), "%s.defoldtmp_%d", save_path, i);
            f = savecache_open_write(tmp);
            assert(f);
            fprintf(f, "n=%d", i);
            assert(savecache_close(f, &r));
            assert(savecache_rename(tmp, save_path));
        }
        savecache_flush();
        assert(!strcmp(read_card(save_path), "n=199"));
        snprintf(tmp, sizeof(tmp), "%s.defoldtmp_199", save_path);
        assert(access(tmp, F_OK) != 0 && savecache_stat(tmp, NULL) == 0);
    }

    puts("savecache: ok");
    return 0;
}
