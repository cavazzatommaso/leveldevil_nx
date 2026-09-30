/* dl_bridge.h -- dlopen/dlsym/dl_iterate_phdr for the loaded modules.
 *
 * Who calls dlopen here, and why it matters:
 *
 *  - hxcpp's CFFI loader (in libApplicationMain.so) dlopen()s liblime and
 *    dlsym()s every lime_*__prime entry point plus hx_set_loader. Without a
 *    working dlsym the game cannot call a single Lime native function.
 *
 *  - SDL's Android video driver dlopen()s "libEGL.so" and "libGLESv2.so" and
 *    dlsym()s each entry point it needs. That is how EGL reaches switch-mesa
 *    here, and it is the reason imports.c carries egl* rows for functions no
 *    ELF header actually imports: without them SDL_EGL_LoadLibrary finds
 *    nothing and the game never gets a GL context.
 *
 *    This build has SDL's Dynamic API compiled out, so there is no
 *    SDL_DYNAPI_entry to answer and no jump table to fill -- the Android SDL
 *    inside liblime is the SDL that runs. See android_sdl.c.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef PB_DL_BRIDGE_H
#define PB_DL_BRIDGE_H

#include <stddef.h>

void *bx_dlopen(const char *name, int flags);
void *bx_dlsym(void *handle, const char *name);
int   bx_dlclose(void *handle);
char *bx_dlerror(void);
int   bx_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data);

#endif
