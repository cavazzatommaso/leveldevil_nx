/* paths.c -- see paths.h. MIT licensed, see LICENSE. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "paths.h"

/* P holds a folder, Q a path derived from one, R a path derived from a
 * derived one. Each step up leaves room for the suffix, so the compiler can
 * see that no snprintf here can truncate. */
#define P 768
#define Q 1024
#define R 1280

static char g_root[P], g_root_nodev[P];
static char g_assets[Q], g_assets_nodev[Q], g_save[Q], g_save_nodev[Q];
static char g_lib_lime[Q], g_lib_app[Q], g_config[Q], g_log[Q];
static char g_prefs[R], g_sdcard_nodev[R];
static char g_tried[1024];

const char *paths_root(void)         { return g_root; }
const char *paths_assets(void)       { return g_assets; }
const char *paths_assets_nodev(void) { return g_assets_nodev; }
const char *paths_save(void)         { return g_save; }
const char *paths_save_nodev(void)   { return g_save_nodev; }
const char *paths_lib_lime(void)     { return g_lib_lime; }
const char *paths_lib_app(void)      { return g_lib_app; }
const char *paths_config(void)       { return g_config; }
const char *paths_log(void)          { return g_log; }
const char *paths_prefs(void)        { return g_prefs; }

static int exists(const char *p, int want_dir)
{
    struct stat st;
    if (stat(p, &st) != 0)
        return 0;
    return want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
}

static void strip_device(const char *in, char *out, size_t n)
{
    const char *colon = strchr(in, ':'), *slash = strchr(in, '/');
    if (colon && (!slash || colon < slash))
        in = colon + 1;
    snprintf(out, n, "%s", in);
}

/* Does this folder hold the game? If so, remember where everything is. */
static int layout_ok(const char *dir)
{
    char lime[Q], app[Q], assets[Q], probe[R];

    snprintf(lime, sizeof(lime), "%s/lib/liblime.so", dir);
    snprintf(app, sizeof(app), "%s/lib/libApplicationMain.so", dir);
    if (!exists(lime, 0) || !exists(app, 0)) {
        snprintf(lime, sizeof(lime), "%s/liblime.so", dir);
        snprintf(app, sizeof(app), "%s/libApplicationMain.so", dir);
        if (!exists(lime, 0) || !exists(app, 0))
            return 0;
    }

    snprintf(assets, sizeof(assets), "%s/assets", dir);
    snprintf(probe, sizeof(probe), "%s/manifest/default.json", assets);
    if (!exists(probe, 0)) {
        snprintf(probe, sizeof(probe), "%s/manifest/default.json", dir);
        if (!exists(probe, 0))
            return 0;
        snprintf(assets, sizeof(assets), "%s", dir);
    }

    snprintf(g_root, sizeof(g_root), "%s", dir);
    snprintf(g_assets, sizeof(g_assets), "%s", assets);
    snprintf(g_lib_lime, sizeof(g_lib_lime), "%s", lime);
    snprintf(g_lib_app, sizeof(g_lib_app), "%s", app);
    snprintf(g_save, sizeof(g_save), "%s/save", g_root);
    snprintf(g_config, sizeof(g_config), "%s/config.txt", g_root);
    snprintf(g_log, sizeof(g_log), "%s/leveldevil.log", g_root);
    snprintf(g_prefs, sizeof(g_prefs), "%s/prefs.txt", g_save);
    strip_device(g_root, g_root_nodev, sizeof(g_root_nodev));
    strip_device(g_assets, g_assets_nodev, sizeof(g_assets_nodev));
    strip_device(g_save, g_save_nodev, sizeof(g_save_nodev));
    snprintf(g_sdcard_nodev, sizeof(g_sdcard_nodev), "%s/sdcard", g_save_nodev);
    return 1;
}

static int try_dir(const char *dir)
{
    size_t len = strlen(g_tried);
    if (len < sizeof(g_tried) - 80)
        snprintf(g_tried + len, sizeof(g_tried) - len, "%s  %s", len ? "\n" : "", dir);
    return layout_ok(dir);
}

int paths_locate(int argc, char **argv, char *err, size_t errlen)
{
    static const char *const sub_names[] = { "leveldevil", "leveldevil_nx", "hs" };
    static const char *const known[] = {
        "sdmc:/switch/leveldevil", "sdmc:/switch/leveldevil_nx",
        "sdmc:/switch/hs", "sdmc:/leveldevil", "sdmc:/switch",
    };
    char dir[P], sub[Q];
    size_t i;
    DIR *d;

    g_tried[0] = '\0';

    /* 1. next to the .nro itself (argv[0] from the Homebrew Menu) */
    if (argc > 0 && argv && argv[0] && strchr(argv[0], '/')) {
        char *slash;
        snprintf(dir, sizeof(dir), "%s", argv[0]);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            if (try_dir(dir))
                return 1;
            for (i = 0; i < sizeof(sub_names) / sizeof(sub_names[0]); i++) {
                snprintf(sub, sizeof(sub), "%s/%s", dir, sub_names[i]);
                if (try_dir(sub))
                    return 1;
            }
        }
    }

    /* 2. the usual places */
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (try_dir(known[i]))
            return 1;

    /* 3. every folder in sdmc:/switch */
    d = opendir("sdmc:/switch");
    if (d) {
        struct dirent *e;
        int scanned = 0;
        while ((e = readdir(d)) != NULL && scanned < 256) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(sub, sizeof(sub), "sdmc:/switch/%s", e->d_name);
            if (!exists(sub, 1))
                continue;
            scanned++;
            if (layout_ok(sub)) {
                closedir(d);
                return 1;
            }
        }
        closedir(d);
        if (scanned) {
            size_t len = strlen(g_tried);
            snprintf(g_tried + len, sizeof(g_tried) - len,
                     "\n  (and %d folder(s) inside sdmc:/switch)", scanned);
        }
    }

    snprintf(err, errlen,
             "The game files were not found.\n\n"
             "Put these next to leveldevil.nro, in any folder:\n"
             "  liblime.so             (from the APK's lib/arm64-v8a)\n"
             "  libApplicationMain.so  (same folder in the APK)\n"
             "  assets                 (the APK's assets folder)\n\n"
             "Looked in:\n%s", g_tried);
    return 0;
}

int paths_init(char *err, size_t errlen)
{
    char probe[R];

    mkdir(g_save, 0777);
    if (!exists(g_save, 1)) {
        snprintf(err, errlen, "Could not create %s\n(SD card write protected or full?)", g_save);
        return 0;
    }
    if (chdir(g_assets) != 0) {
        snprintf(err, errlen, "chdir(%s) failed (errno %d)", g_assets, errno);
        return 0;
    }

    LOGI("paths: game folder %s", g_root);
    LOGI("paths: liblime %s", g_lib_lime);
    LOGI("paths: assets %s (working directory)", g_assets);
    LOGI("paths: saves %s", g_save_nodev);

    snprintf(probe, sizeof(probe), "%s/assets/data/game.mbs", g_assets);
    if (!exists(probe, 0))
        LOGE("paths: %s is missing -- if this is not Level Devil, expect trouble", probe);
    return 1;
}

typedef struct { const char *from; const char *to; } PrefixMap;

/* Longest prefixes first; the targets are filled in once the root is known. */
static PrefixMap prefixes(size_t i)
{
    static const char *const from[] = {
        "/storage/emulated/0/Android/data/" HS_PACKAGE "/files",
        "/sdcard/Android/data/" HS_PACKAGE "/files",
        "/data/data/" HS_PACKAGE "/files",
        "/data/user/0/" HS_PACKAGE "/files",
        "/data/data/" HS_PACKAGE,
        "/data/user/0/" HS_PACKAGE,
        "/storage/emulated/0",
        "/sdcard",
        "/android_asset",
    };
    PrefixMap m;
    m.from = from[i];
    m.to = (i == 8) ? g_assets_nodev : (i >= 6 ? g_sdcard_nodev : g_save_nodev);
    return m;
}
#define PREFIX_COUNT 9

static void normalize(const char *in, char *out, size_t outlen)
{
    /* Split into [device:] [/] components, drop "" and ".", resolve ".." */
    const char *comp[128];
    size_t clen[128];
    int n = 0, absolute = 0;
    size_t devlen = 0, pos = 0;
    const char *p = in, *colon = strchr(in, ':'), *slash = strchr(in, '/');

    if (colon && (!slash || colon < slash))
        devlen = (size_t)(colon - in) + 1;
    p = in + devlen;
    if (*p == '/')
        absolute = 1;

    while (*p) {
        const char *s;
        size_t len;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (n > 0 && !(clen[n - 1] == 2 && comp[n - 1][0] == '.' && comp[n - 1][1] == '.')) {
                n--;
                continue;
            }
            if (absolute)
                continue;               /* cannot go above the root */
        }
        if (n < 128) {
            comp[n] = s;
            clen[n] = len;
            n++;
        }
    }

    if (outlen == 0)
        return;
#define PUT(ch) do { if (pos + 1 < outlen) out[pos++] = (ch); } while (0)
    for (size_t i = 0; i < devlen; i++)
        PUT(in[i]);
    if (absolute)
        PUT('/');
    for (int i = 0; i < n; i++) {
        if (i > 0)
            PUT('/');
        for (size_t j = 0; j < clen[i]; j++)
            PUT(comp[i][j]);
    }
    if (pos == 0)
        PUT('.');
#undef PUT
    out[pos] = '\0';
}

const char *path_translate(const char *in, char *out, size_t outlen)
{
    char tmp[R + 1024];
    size_t i;

    if (!in) {
        if (outlen)
            out[0] = '\0';
        return out;
    }
    if (!strncmp(in, "file://", 7))
        in += 7;

    snprintf(tmp, sizeof(tmp), "%s", in);
    for (i = 0; i < PREFIX_COUNT; i++) {
        PrefixMap m = prefixes(i);
        size_t fl = strlen(m.from);
        if (!strncmp(in, m.from, fl) && (in[fl] == '\0' || in[fl] == '/')) {
            snprintf(tmp, sizeof(tmp), "%s%s", m.to, in + fl);
            LOGD("path: %s -> %s", in, tmp);
            break;
        }
    }
    normalize(tmp, out, outlen);
    return out;
}
