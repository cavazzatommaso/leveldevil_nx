/* assetmgr.c -- the NDK asset manager, over the assets folder on the SD card.
 *
 * From the Poor Bunny port. Total Party Kill's Lime read its assets with
 * plain fopen() on paths relative to the APK's assets folder, so making the
 * working directory that folder was enough. This build imports the NDK asset
 * API instead --
 *
 *     AAssetManager_fromJava, AAssetManager_open, AAsset_read, AAsset_seek64,
 *     AAsset_getLength64, AAsset_openFileDescriptor, AAsset_close
 *
 * -- so Lime asks an object for its files rather than the filesystem, and the
 * port has to be that object. Every name is resolved against the staged
 * assets folder; an asset is just a FILE.
 *
 * Name resolution is deliberately forgiving. The manifest this build ships
 * carries rootPath "../", Lime joins that onto asset paths in some code
 * paths and not others, and the folder on the card may hold the APK's
 * assets/ directory whole or its contents loose. So a name is normalised
 * (".", "..", "//", a "file://" prefix and any leading slash are folded away)
 * and then tried against the assets root, with and without a leading
 * "assets/", before it is called missing. A miss is logged once per name,
 * which is what turns "the game booted to a black screen" into a line that
 * names the file it wanted.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_stubs.h"
#include "assetmgr.h"
#include "log.h"
#include "paths.h"

#define AASSET_MODE_UNKNOWN   0
#define AASSET_MODE_RANDOM    1
#define AASSET_MODE_STREAMING 2
#define AASSET_MODE_BUFFER    3

/* A single dummy manager object: the handle SDL/Lime pass around carries no
 * state of its own, since there is only one place assets can come from. */
static int g_manager_token = 0xA55E7;

typedef struct {
    FILE    *fp;
    long     length;
    void    *buffer;          /* AAsset_getBuffer: the whole file, read once */
    char     name[256];
} PbAsset;

/* ------------------------------------------------------------ naming ---- */

/* Collapse ".", "..", repeated slashes and a "file://" or leading "/" into a
 * clean relative path. Always NUL-terminates. */
static void normalize(const char *in, char *out, size_t outlen)
{
    const char *p = in;
    char *segs[64];
    char work[1024];
    char *save = NULL;
    size_t nseg = 0, i, len = 0;

    if (!out || !outlen)
        return;
    out[0] = '\0';
    if (!p)
        return;
    if (!strncmp(p, "file://", 7))
        p += 7;
    while (*p == '/')
        p++;

    snprintf(work, sizeof(work), "%s", p);
    /* strtok_r, not strtok: Lime opens assets from more than one thread and a
     * shared parser state here would corrupt paths under load. */
    for (char *tok = strtok_r(work, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".") || !*tok)
            continue;
        if (!strcmp(tok, "..")) {
            if (nseg)
                nseg--;            /* a leading ".." simply falls off */
            continue;
        }
        if (nseg < sizeof(segs) / sizeof(segs[0]))
            segs[nseg++] = tok;
    }
    for (i = 0; i < nseg; i++) {
        int n = snprintf(out + len, outlen - len, "%s%s", len ? "/" : "", segs[i]);
        if (n < 0 || (size_t)n >= outlen - len)
            break;
        len += (size_t)n;
    }
}

/* Try the candidates in order; returns an open FILE* or NULL. */
static FILE *open_asset(const char *name, char *chosen, size_t chosenlen)
{
    char clean[1024], full[1024];
    const char *roots[2];
    int r;

    normalize(name, clean, sizeof(clean));
    if (!clean[0])
        return NULL;

    roots[0] = paths_assets();
    roots[1] = paths_root();

    for (r = 0; r < 2; r++) {
        int variant;
        for (variant = 0; variant < 2; variant++) {
            FILE *fp;
            const char *rel = clean;
            /* variant 1 drops a leading "assets/" so that a card holding the
             * APK's assets/ contents loose still answers "assets/data/x". */
            if (variant == 1) {
                if (strncmp(clean, "assets/", 7) != 0)
                    continue;
                rel = clean + 7;
            }
            snprintf(full, sizeof(full), "%s/%s", roots[r], rel);
            fp = fopen(full, "rb");
            if (fp) {
                snprintf(chosen, chosenlen, "%s", full);
                return fp;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------- imported AAsset API --- */

void *ax_AAssetManager_fromJava(void *env, void *assetManager)
{
    (void)env; (void)assetManager;
    return &g_manager_token;
}

void *ax_AAssetManager_open(void *mgr, const char *filename, int mode)
{
    PbAsset *a;
    char chosen[1024];
    FILE *fp;

    (void)mgr; (void)mode;
    if (!filename)
        return NULL;

    fp = open_asset(filename, chosen, sizeof(chosen));
    if (!fp) {
        LOG_ONCE("assets: \"%s\" is not in %s", filename, paths_assets());
        return NULL;
    }

    a = calloc(1, sizeof(*a));
    if (!a) {
        fclose(fp);
        return NULL;
    }
    a->fp = fp;
    fseek(fp, 0, SEEK_END);
    a->length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    snprintf(a->name, sizeof(a->name), "%s", filename);
    LOGD("assets: open %s -> %s (%ld bytes)", filename, chosen, a->length);
    return a;
}

int ax_AAsset_read(void *asset, void *buf, size_t count)
{
    PbAsset *a = asset;
    size_t n;

    if (!a || !buf)
        return -1;
    n = fread(buf, 1, count, a->fp);
    if (n == 0 && ferror(a->fp))
        return -1;
    return (int)n;
}

int64_t ax_AAsset_seek64(void *asset, int64_t offset, int whence)
{
    PbAsset *a = asset;

    if (!a)
        return -1;
    if (fseek(a->fp, (long)offset, whence) != 0)
        return -1;
    return (int64_t)ftell(a->fp);
}

int64_t ax_AAsset_getLength64(void *asset)
{
    PbAsset *a = asset;
    return a ? (int64_t)a->length : 0;
}

void ax_AAsset_close(void *asset)
{
    PbAsset *a = asset;

    if (!a)
        return;
    if (a->fp)
        fclose(a->fp);
    free(a->buffer);
    free(a);
}

/* On Android this returns a descriptor into the APK, so the caller is handed
 * an offset and a length inside a much larger file. Here the asset is its own
 * file, so the offset is zero and the length is the whole of it. */
int ax_AAsset_openFileDescriptor(void *asset, int64_t *outStart, int64_t *outLength)
{
    PbAsset *a = asset;
    int fd;

    if (!a || !a->fp)
        return -1;
    fd = dup(fileno(a->fp));
    if (fd < 0) {
        LOG_ONCE("assets: could not duplicate a descriptor for \"%s\"", a->name);
        return -1;
    }
    if (outStart)
        *outStart = 0;
    if (outLength)
        *outLength = a->length;
    return fd;
}

/* Defold reads its archive (game.arcd, game.arci...) this way. The asset is
 * its own file here, so "the buffer" is the file read into memory once and
 * kept until AAsset_close. */
const void *ax_AAsset_getBuffer(void *asset)
{
    PbAsset *a = asset;
    long pos;
    if (!a || !a->fp)
        return NULL;
    if (a->buffer)
        return a->buffer;
    a->buffer = malloc(a->length ? (size_t)a->length : 1);
    if (!a->buffer) {
        LOGE("assets: out of memory buffering \"%s\" (%ld bytes)", a->name, a->length);
        return NULL;
    }
    pos = ftell(a->fp);
    fseek(a->fp, 0, SEEK_SET);
    if (fread(a->buffer, 1, (size_t)a->length, a->fp) != (size_t)a->length) {
        LOGE("assets: short read buffering \"%s\"", a->name);
        free(a->buffer);
        a->buffer = NULL;
    }
    fseek(a->fp, pos, SEEK_SET);
    return a->buffer;
}

long ax_AAsset_getLength(void *asset)                   { return (long)ax_AAsset_getLength64(asset); }
long ax_AAsset_seek(void *asset, long off, int whence) { return (long)ax_AAsset_seek64(asset, off, whence); }

/* ------------------------------------------------------ AConfiguration --- */
/* The glue prints the whole configuration at start-up; Defold then reads the
 * locale. Values are a landscape 720p touchscreen device. */

static int g_config_token = 0xC0F16;

void *ax_AConfiguration_new(void)                    { return &g_config_token; }
void  ax_AConfiguration_delete(void *c)              { (void)c; }
void  ax_AConfiguration_fromAssetManager(void *c, void *m) { (void)c; (void)m; }

void ax_AConfiguration_getLanguage(void *c, char *out)
{
    (void)c;
    if (out) { out[0] = 'e'; out[1] = 'n'; out[2] = '\0'; }
}

void ax_AConfiguration_getCountry(void *c, char *out)
{
    (void)c;
    if (out) { out[0] = 'U'; out[1] = 'S'; out[2] = '\0'; }
}

int32_t ax_AConfiguration_getDensity(void *c)      { (void)c; return 160; }   /* DENSITY_MEDIUM */
int32_t ax_AConfiguration_getKeyboard(void *c)     { (void)c; return 2; }     /* QWERTY */
int32_t ax_AConfiguration_getKeysHidden(void *c)   { (void)c; return 1; }     /* NO */
int32_t ax_AConfiguration_getMcc(void *c)          { (void)c; return 0; }
int32_t ax_AConfiguration_getMnc(void *c)          { (void)c; return 0; }
int32_t ax_AConfiguration_getNavHidden(void *c)    { (void)c; return 1; }
int32_t ax_AConfiguration_getNavigation(void *c)   { (void)c; return 2; }     /* DPAD */
int32_t ax_AConfiguration_getOrientation(void *c)  { (void)c; return 2; }     /* LAND */
int32_t ax_AConfiguration_getScreenLong(void *c)   { (void)c; return 2; }     /* YES */
int32_t ax_AConfiguration_getScreenSize(void *c)   { (void)c; return 3; }     /* LARGE */
int32_t ax_AConfiguration_getSdkVersion(void *c)   { (void)c; return 29; }
int32_t ax_AConfiguration_getTouchscreen(void *c)  { (void)c; return 3; }     /* FINGER */
int32_t ax_AConfiguration_getUiModeNight(void *c)  { (void)c; return 1; }     /* NO */
int32_t ax_AConfiguration_getUiModeType(void *c)   { (void)c; return 1; }     /* NORMAL */
