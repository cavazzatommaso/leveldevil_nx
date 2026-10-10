/* savecache.h -- see savecache.c. Paths are as path_translate leaves them
 * (no device prefix). MIT licensed, see LICENSE. */
#ifndef LD_SAVECACHE_H
#define LD_SAVECACHE_H

#include <stddef.h>
#include <stdio.h>

int   savecache_handles(const char *p);          /* inside save/? */
FILE *savecache_open_write(const char *p);       /* NULL: use the card */
FILE *savecache_open_read(const char *p);        /* for a stat()==1 path */
int   savecache_close(FILE *f, int *ret);        /* 1 if f was ours */
int   savecache_rename(const char *a, const char *b);   /* 1 if handled */
void  savecache_unlink(const char *p);
int   savecache_stat(const char *p, size_t *size);      /* 1 cached, -1 deleted, 0 unknown */
void  savecache_flush(void);                     /* at exit: wait for the writer */

#endif
