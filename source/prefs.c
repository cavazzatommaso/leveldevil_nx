/* prefs.c -- SharedPreferences("nmeAppPrefs") for com.androidnative.Native.
 *
 * A flat key=value file in save/prefs.txt, rewritten on every change the way
 * SharedPreferences.Editor.commit() persists synchronously. Backslash escapes
 * keep keys and values containing '=', newlines or backslashes intact.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "log.h"
#include "paths.h"
#include "prefs.h"

typedef struct Pref {
    char *key, *value;
    struct Pref *next;
} Pref;

static Pref  *g_prefs;
static int    g_loaded;
static RMutex g_lock;

static char *unescape(const char *s, size_t len)
{
    char *out = malloc(len + 1), *p = out;
    size_t i;
    if (!out)
        return NULL;
    for (i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            i++;
            *p++ = s[i] == 'n' ? '\n' : (s[i] == 'r' ? '\r' : (s[i] == 'e' ? '=' : s[i]));
        } else {
            *p++ = s[i];
        }
    }
    *p = '\0';
    return out;
}

static void write_escaped(FILE *fp, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '=':  fputs("\\e", fp); break;
        case '\\': fputs("\\\\", fp); break;
        default:   fputc(*s, fp); break;
        }
    }
}

static void load_locked(void)
{
    FILE *fp;
    char line[4096];
    if (g_loaded)
        return;
    g_loaded = 1;
    fp = fopen(paths_prefs(), "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        char *eq;
        Pref *p;
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        eq = strchr(line, '=');
        if (!eq)
            continue;
        p = calloc(1, sizeof(*p));
        if (!p)
            break;
        p->key = unescape(line, (size_t)(eq - line));
        p->value = unescape(eq + 1, strlen(eq + 1));
        p->next = g_prefs;
        g_prefs = p;
    }
    fclose(fp);
}

static void save_locked(void)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", paths_prefs());
    FILE *fp = fopen(tmp, "w");
    Pref *p;
    if (!fp) {
        LOGE("prefs: cannot write %s", tmp);
        return;
    }
    for (p = g_prefs; p; p = p->next) {
        write_escaped(fp, p->key);
        fputc('=', fp);
        write_escaped(fp, p->value);
        fputc('\n', fp);
    }
    fclose(fp);
    unlink(paths_prefs());
    if (rename(tmp, paths_prefs()) != 0)
        LOGE("prefs: rename failed");
}

char *prefs_get(const char *key)
{
    Pref *p;
    char *r = NULL;
    if (!key)
        return strdup("");
    rmutexLock(&g_lock);
    load_locked();
    for (p = g_prefs; p; p = p->next)
        if (!strcmp(p->key, key))
            break;
    r = strdup(p ? p->value : "");
    rmutexUnlock(&g_lock);
    return r;
}

void prefs_set(const char *key, const char *value)
{
    Pref *p;
    if (!key)
        return;
    if (!value)
        value = "";
    rmutexLock(&g_lock);
    load_locked();
    for (p = g_prefs; p; p = p->next)
        if (!strcmp(p->key, key))
            break;
    if (p) {
        if (!strcmp(p->value, value)) {
            rmutexUnlock(&g_lock);
            return;
        }
        free(p->value);
        p->value = strdup(value);
    } else {
        p = calloc(1, sizeof(*p));
        if (!p) {
            rmutexUnlock(&g_lock);
            return;
        }
        p->key = strdup(key);
        p->value = strdup(value);
        p->next = g_prefs;
        g_prefs = p;
    }
    save_locked();
    rmutexUnlock(&g_lock);
    LOGD("prefs: %s = %s", key, value);
}
