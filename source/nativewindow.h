/* nativewindow.h -- see nativewindow.c. MIT licensed, see LICENSE. */
#ifndef PB_NATIVEWINDOW_H
#define PB_NATIVEWINDOW_H

/* Take the default NWindow and size it. Call before liblime's constructors. */
int   pbnw_init(void);
int   pbnw_resize(int w, int h);
/* Re-size to match dock state when resolution is auto. 1 if it changed. */
int   pbnw_follow_operation_mode(void);

void *pbnw_handle(void);     /* the NWindow*, which is also the ANativeWindow* */
int   pbnw_width(void);
int   pbnw_height(void);
int   pbnw_is_ready(void);

/* Put our crop and transform back after mesa has built its swapchain. */
void  pbnw_reassert(void);
/* Log what mesa actually built: buffer count, swap interval, and whether the
 * compositor says it is running behind. */
void  pbnw_report(const char *when);
/* frame_stats: slow frames and worst frame time since the last call. */
void  pbnw_frame_stats(unsigned *slow, unsigned *worst_ms);

/* Interposed in imports.c: the Switch has one NWindow and therefore exactly
 * one swapchain, so the window surface is a singleton. See nativewindow.c. */
#include <EGL/egl.h>
EGLSurface pbnw_egl_create_window_surface(EGLDisplay dpy, EGLConfig cfg,
                                          void *native, const EGLint *attribs);
EGLBoolean pbnw_egl_destroy_surface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean pbnw_egl_swap_buffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean pbnw_egl_query_surface(EGLDisplay dpy, EGLSurface surface, EGLint attr, EGLint *value);
EGLDisplay pbnw_egl_get_platform_display(unsigned int platform, void *native_display,
                                         const void *attrib_list);

#endif
