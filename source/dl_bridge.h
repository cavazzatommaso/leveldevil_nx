/* dl_bridge.h -- dlopen/dlsym/dl_iterate_phdr for the loaded modules.
 *
 * Who calls dlopen here, and why it matters:
 *
 *  - hxcpp's CFFI loader (in libApplicationMain.so) dlopen()s liblime and
 *    dlsym()s every lime_*__prime entry point plus hx_set_loader. Without a
 *    working dlsym the game cannot call a single Lime native function.
 *
 *  - SDL's Dynamic API (in liblime.so) reads $SDL_DYNAMIC_API, dlopen()s that
 *    name and calls its SDL_DYNAPI_entry. The environment is seeded with
 *    HS_SDL_SENTINEL so that lookup lands in sdl_bridge.c, which fills
 *    liblime's jump table with switch-sdl2.
 *
 *  - SDL and OpenAL probe libGLESv2/libEGL/libOpenSLES; those resolve
 *    through the import table so the probes see the same functions the
 *    modules were linked against.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_DL_BRIDGE_H
#define HS_DL_BRIDGE_H

#include <stddef.h>

#define HS_SDL_SENTINEL "libSDL2-switch-native.so"

void *bx_dlopen(const char *name, int flags);
void *bx_dlsym(void *handle, const char *name);
int   bx_dlclose(void *handle);
char *bx_dlerror(void);
int   bx_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data);

#endif
