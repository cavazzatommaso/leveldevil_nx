/* dl_bridge.c -- see dl_bridge.h. MIT licensed, see LICENSE. */
#include <stdio.h>
#include <string.h>

#include "dl_bridge.h"
#include "imports.h"
#include "log.h"
#include "sdl_bridge.h"
#include "so_util.h"

/* Opaque handles for the non-module cases. Never dereferenced. */
static char g_handle_global, g_handle_system, g_handle_sdl;
#define H_GLOBAL ((void *)&g_handle_global)
#define H_SYSTEM ((void *)&g_handle_system)
#define H_SDL    ((void *)&g_handle_sdl)

static __thread const char *t_error;
static __thread char t_errbuf[256];

static void set_error(const char *fmt, const char *arg)
{
    snprintf(t_errbuf, sizeof(t_errbuf), fmt, arg);
    t_error = t_errbuf;
}

/* "dir/liblime.so" and "lime.ndll" and "lime" all reduce to "lime". */
static void stem(const char *in, char *out, size_t n)
{
    const char *b = strrchr(in, '/');
    size_t len;
    const char *dot;
    b = b ? b + 1 : in;
    if (!strncmp(b, "lib", 3))
        b += 3;
    dot = strchr(b, '.');
    len = dot ? (size_t)(dot - b) : strlen(b);
    if (len >= n)
        len = n - 1;
    memcpy(out, b, len);
    out[len] = '\0';
}

static so_module *module_from_handle(void *h)
{
    so_module *m;
    for (m = so_module_list(); m; m = m->next)
        if ((void *)m == h)
            return m;
    return NULL;
}

void *bx_dlopen(const char *name, int flags)
{
    static const char *const system_libs[] = {
        "c", "m", "dl", "log", "android", "EGL", "GLESv1_CM", "GLESv2", "GLESv3",
        "OpenSLES", "stdc++", "c++_shared", "z", "jnigraphics",
    };
    char want[128], have[128];
    so_module *m;
    size_t i;
    (void)flags;

    if (!name)
        return H_GLOBAL;
    if (strstr(name, HS_SDL_SENTINEL)) {
        LOGI("dlopen(%s): SDL dynamic API -> switch-sdl2", name);
        return H_SDL;
    }
    stem(name, want, sizeof(want));
    for (m = so_module_list(); m; m = m->next) {
        stem(m->name, have, sizeof(have));
        if (!strcmp(want, have)) {
            LOGI("dlopen(%s) -> %s", name, m->name);
            return m;
        }
    }
    for (i = 0; i < sizeof(system_libs) / sizeof(system_libs[0]); i++) {
        if (!strcmp(want, system_libs[i])) {
            LOGD("dlopen(%s) -> system library table", name);
            return H_SYSTEM;
        }
    }
    LOGI("dlopen(%s): not available", name);
    set_error("dlopen failed: library \"%s\" not found", name);
    return NULL;
}

void *bx_dlsym(void *handle, const char *name)
{
    uintptr_t addr = 0;
    so_module *m;

    if (!name) {
        t_error = "dlsym: NULL symbol name";
        return NULL;
    }
    if (handle == H_SDL) {
        if (!strcmp(name, "SDL_DYNAPI_entry"))
            return (void *)&hs_SDL_DYNAPI_entry;
    } else if (handle == H_SYSTEM) {
        addr = imports_lookup(name);
    } else if (handle == H_GLOBAL || handle == NULL || handle == (void *)-1L) {
        /* RTLD_DEFAULT (NULL) and RTLD_NEXT (-1): modules, then the table */
        for (m = so_module_list(); m && !addr; m = m->next)
            addr = so_symbol(m, name);
        if (!addr)
            addr = imports_lookup(name);
    } else if ((m = module_from_handle(handle)) != NULL) {
        addr = so_symbol(m, name);
    } else {
        t_error = "dlsym: invalid handle";
        return NULL;
    }
    if (!addr) {
        set_error("undefined symbol: %s", name);
        LOGD("dlsym(%s): not found", name);
    }
    return (void *)addr;
}

int bx_dlclose(void *handle)
{
    (void)handle;                       /* modules are never unloaded */
    return 0;
}

char *bx_dlerror(void)
{
    const char *e = t_error;
    t_error = NULL;
    return (char *)e;
}

int bx_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data)
{
    return so_dl_iterate_phdr(cb, data);
}
